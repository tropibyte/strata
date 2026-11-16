// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The LZ block compressor, bloom filters and the LRU cache.

#include <atomic>
#include <thread>

#include "doctest.h"
#include "support/test_util.hpp"
#include "util/bloom.hpp"
#include "util/cache.hpp"
#include "util/coding.hpp"
#include "util/lz.hpp"
#include "util/random.hpp"

using namespace strata;

namespace {

std::string compressible(Random& rnd, std::size_t len) {
    std::string base = rnd.printable(20);
    std::string s;
    while (s.size() < len) {
        if (rnd.one_in(3)) {
            s += rnd.printable(rnd.uniform(10) + 1);
        } else {
            s += base.substr(0, rnd.uniform(base.size()) + 1);
        }
    }
    s.resize(len);
    return s;
}

void round_trip(std::string_view input) {
    std::string compressed;
    lz::compress(input, compressed);
    std::size_t len = 0;
    REQUIRE(lz::uncompressed_length(compressed, &len));
    CHECK(len == input.size());
    std::string output;
    REQUIRE(lz::decompress(compressed, output));
    CHECK(output == input);
}

}  // namespace

TEST_SUITE("codecs") {
    TEST_CASE("lz round-trips edge cases") {
        round_trip("");
        round_trip("a");
        round_trip("abc");
        round_trip("abcd");
        round_trip("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        round_trip(std::string(100000, 'z'));
        round_trip("abcabcabcabcabcabcabcabcabcabc");
        std::string all;
        for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
        round_trip(all);
        round_trip(all + all + all);
    }

    TEST_CASE("lz round-trips random and compressible data") {
        Random rnd(1234);
        for (int trial = 0; trial < 300; ++trial) {
            const std::size_t len = static_cast<std::size_t>(rnd.skewed(16));
            round_trip(rnd.one_in(2) ? compressible(rnd, len) : test::random_string(rnd, len));
        }
    }

    TEST_CASE("lz shrinks repetitive data") {
        Random rnd(5);
        const std::string input = compressible(rnd, 64 * 1024);
        std::string compressed;
        lz::compress(input, compressed);
        CHECK(compressed.size() < input.size() / 2);

        std::string runs(64 * 1024, 'x');
        compressed.clear();
        lz::compress(runs, compressed);
        CHECK(compressed.size() < 400);
    }

    TEST_CASE("lz rejects damaged input without crashing") {
        Random rnd(77);
        const std::string input = compressible(rnd, 5000);
        std::string compressed;
        lz::compress(input, compressed);
        std::string output;

        // Every truncation fails cleanly.
        for (std::size_t len = 0; len < compressed.size(); len += 7) {
            CHECK_FALSE(lz::decompress(std::string_view(compressed.data(), len), output));
        }
        // Trailing garbage is rejected.
        CHECK_FALSE(lz::decompress(compressed + "x", output));

        // Random byte flips must never crash or over-read (run under ASan).
        for (int trial = 0; trial < 2000; ++trial) {
            std::string damaged = compressed;
            const int flips = 1 + static_cast<int>(rnd.uniform(4));
            for (int f = 0; f < flips; ++f) {
                damaged[rnd.uniform(damaged.size())] = static_cast<char>(rnd.uniform(256));
            }
            (void)lz::decompress(damaged, output);
        }

        // A header claiming an absurd length is refused before allocating.
        std::string huge;
        put_varint32(huge, 0xffffffffu);
        huge +=
            "\x10"
            "a";
        CHECK_FALSE(lz::decompress(huge, output));

        // A match pointing before the start of the output is refused.
        std::string bad_offset;
        put_varint32(bad_offset, 8);
        bad_offset += std::string(
            "\x14"
            "abcd"
            "\x10\x00",
            7);  // 1 literal... offset 16 > produced
        CHECK_FALSE(lz::decompress(bad_offset, output));
    }

    TEST_CASE("bloom filter has no false negatives") {
        for (int bits : {1, 4, 10, 20}) {
            BloomFilterBuilder builder(bits);
            for (int i = 0; i < 1000; ++i) builder.add_key(test::key(static_cast<std::uint64_t>(i)));
            const std::string filter = builder.finish();
            for (int i = 0; i < 1000; ++i) CHECK(bloom_may_match(filter, test::key(static_cast<std::uint64_t>(i))));
        }
    }

    TEST_CASE("bloom filter false-positive rate is about right") {
        for (int n : {1, 10, 100, 1000, 10000}) {
            BloomFilterBuilder builder(10);
            for (int i = 0; i < n; ++i) builder.add_key(test::key(static_cast<std::uint64_t>(i)));
            const std::string filter = builder.finish();
            CHECK(filter.size() <= static_cast<std::size_t>(n) * 10 / 8 + 40);
            int false_positives = 0;
            for (int i = 0; i < 10000; ++i) {
                if (bloom_may_match(filter, test::key(static_cast<std::uint64_t>(i + 1000000000)))) ++false_positives;
            }
            INFO("n = " << n << ", false positives per 10000 = " << false_positives);
            CHECK(false_positives < 250);  // under 2.5%
        }
    }

    TEST_CASE("bloom filter edge cases") {
        BloomFilterBuilder builder(10);
        CHECK(builder.num_probes() == 7);
        const std::string empty = builder.finish();
        CHECK_FALSE(bloom_may_match(empty, "hello"));

        builder.add_key("hello");
        builder.add_key("world");
        const std::string small = builder.finish();
        CHECK(bloom_may_match(small, "hello"));
        CHECK(bloom_may_match(small, "world"));
        CHECK_FALSE(bloom_may_match(small, "x"));
        CHECK_FALSE(bloom_may_match(small, "foo"));

        // Malformed filters match everything rather than hide data.
        CHECK(bloom_may_match("", "anything"));
        CHECK(bloom_may_match(std::string(8, '\0') + "\x40", "anything"));  // k = 64: reserved
        CHECK(builder.num_keys() == 0);                                     // finish() reset it
    }

    TEST_CASE("cache lookup, replace and erase") {
        LruCache<int, std::string> cache(1000, 0);
        CHECK(cache.lookup(1) == nullptr);
        cache.insert(1, std::make_shared<std::string>("one"), 1);
        cache.insert(2, std::make_shared<std::string>("two"), 1);
        REQUIRE(cache.lookup(1) != nullptr);
        CHECK(*cache.lookup(1) == "one");
        cache.insert(1, std::make_shared<std::string>("uno"), 1);
        CHECK(*cache.lookup(1) == "uno");
        CHECK(cache.size() == 2);
        CHECK(cache.total_charge() == 2);
        cache.erase(1);
        CHECK(cache.lookup(1) == nullptr);
        CHECK(cache.size() == 1);
        cache.erase(42);  // absent: no-op
        cache.clear();
        CHECK(cache.size() == 0);
        CHECK(cache.total_charge() == 0);
        CHECK(cache.hits() >= 3);
        CHECK(cache.misses() >= 2);
    }

    TEST_CASE("cache evicts least recently used first") {
        LruCache<int, int> cache(3, 0);  // one shard, three entries
        for (int i = 1; i <= 3; ++i) cache.insert(i, std::make_shared<int>(i), 1);
        REQUIRE(cache.lookup(1) != nullptr);  // 1 is now most recent
        cache.insert(4, std::make_shared<int>(4), 1);
        CHECK(cache.lookup(2) == nullptr);  // 2 was least recent
        CHECK(cache.lookup(1) != nullptr);
        CHECK(cache.lookup(3) != nullptr);
        CHECK(cache.lookup(4) != nullptr);
        CHECK(cache.total_charge() == 3);
    }

    TEST_CASE("cache respects charges and keeps evicted values alive for holders") {
        LruCache<int, std::string> cache(100, 0);
        cache.insert(1, std::make_shared<std::string>("big"), 60);
        auto held = cache.lookup(1);
        cache.insert(2, std::make_shared<std::string>("bigger"), 60);  // evicts 1
        CHECK(cache.lookup(1) == nullptr);
        REQUIRE(held != nullptr);
        CHECK(*held == "big");  // still usable
        CHECK(cache.total_charge() == 60);

        cache.insert(3, std::make_shared<std::string>("huge"), 1000);  // larger than the cache
        CHECK(cache.lookup(3) == nullptr);
        CHECK(cache.total_charge() <= 100);
    }

    TEST_CASE("zero-capacity cache stores nothing") {
        LruCache<int, int> cache(0);
        cache.insert(1, std::make_shared<int>(1), 1);
        CHECK(cache.lookup(1) == nullptr);
        CHECK(cache.new_id() != cache.new_id());
    }

    TEST_CASE("cache is safe under concurrent use") {
        LruCache<int, int> cache(500, 3);
        std::atomic<int> hits{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                Random rnd(static_cast<std::uint64_t>(t) + 1);
                for (int i = 0; i < 20000; ++i) {
                    const int k = static_cast<int>(rnd.uniform(1000));
                    if (auto v = cache.lookup(k)) {
                        if (*v == k) hits.fetch_add(1);
                    } else {
                        cache.insert(k, std::make_shared<int>(k), 1);
                    }
                    if (rnd.one_in(100)) cache.erase(k);
                }
            });
        }
        for (auto& t : threads) t.join();
        CHECK(hits.load() > 0);
        CHECK(cache.total_charge() <= 500 + 8);  // per-shard rounding
    }
}
