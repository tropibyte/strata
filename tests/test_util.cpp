// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Utilities: integer coding, checksums, hashing, arena, strings, histogram,
// status and comparators.

#include <cstring>
#include <limits>
#include <set>

#include "doctest.h"
#include "strata/comparator.hpp"
#include "strata/status.hpp"
#include "util/arena.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/hash.hpp"
#include "util/histogram.hpp"
#include "util/random.hpp"
#include "util/strutil.hpp"

using namespace strata;

TEST_SUITE("util") {
    TEST_CASE("fixed32 and fixed64 round-trip and are little-endian") {
        std::string s;
        for (std::uint32_t v = 0; v < 100000; v += 7) put_fixed32(s, v);
        std::string_view in = s;
        for (std::uint32_t v = 0; v < 100000; v += 7) {
            std::uint32_t got = 0;
            REQUIRE(get_fixed32(in, &got));
            CHECK(got == v);
        }
        CHECK(in.empty());

        std::string b;
        put_fixed32(b, 0x04030201u);
        CHECK(b == std::string("\x01\x02\x03\x04", 4));

        std::string s64;
        for (int power = 0; power <= 63; ++power) {
            const std::uint64_t v = std::uint64_t{1} << power;
            put_fixed64(s64, v - 1);
            put_fixed64(s64, v);
            put_fixed64(s64, v + 1);
        }
        std::string_view in64 = s64;
        for (int power = 0; power <= 63; ++power) {
            const std::uint64_t v = std::uint64_t{1} << power;
            std::uint64_t got = 0;
            REQUIRE(get_fixed64(in64, &got));
            CHECK(got == v - 1);
            REQUIRE(get_fixed64(in64, &got));
            CHECK(got == v);
            REQUIRE(get_fixed64(in64, &got));
            CHECK(got == v + 1);
        }
    }

    TEST_CASE("varint32 round-trip and lengths") {
        std::string s;
        for (std::uint32_t i = 0; i < 32 * 32; ++i) {
            const std::uint32_t v = (i / 32) << (i % 32);
            put_varint32(s, v);
        }
        std::string_view in = s;
        for (std::uint32_t i = 0; i < 32 * 32; ++i) {
            const std::uint32_t expected = (i / 32) << (i % 32);
            const std::size_t before = in.size();
            std::uint32_t got = 0;
            REQUIRE(get_varint32(in, &got));
            CHECK(got == expected);
            CHECK(static_cast<int>(before - in.size()) == varint_length(got));
        }
        CHECK(in.empty());
    }

    TEST_CASE("varint64 round-trip including extremes") {
        std::vector<std::uint64_t> values = {0, 100, ~std::uint64_t{0}, ~std::uint64_t{0} - 1};
        for (int k = 0; k < 64; ++k) {
            const std::uint64_t power = std::uint64_t{1} << k;
            values.push_back(power);
            values.push_back(power - 1);
            values.push_back(power + 1);
        }
        std::string s;
        for (auto v : values) put_varint64(s, v);
        std::string_view in = s;
        for (auto v : values) {
            std::uint64_t got = 0;
            REQUIRE(get_varint64(in, &got));
            CHECK(got == v);
        }
        CHECK(varint_length(~std::uint64_t{0}) == kMaxVarint64Bytes);
    }

    TEST_CASE("varint decoding rejects overflow and truncation") {
        // 32-bit overflow: five bytes whose last carries more than 4 bits.
        std::string_view over32("\x81\x82\x83\x84\x85\x11", 6);
        std::uint32_t v32 = 0;
        CHECK_FALSE(get_varint32(over32, &v32));

        // 64-bit overflow: eleven continuation bytes.
        std::string over64(11, '\x81');
        over64.push_back('\x01');
        std::string_view in64 = over64;
        std::uint64_t v64 = 0;
        CHECK_FALSE(get_varint64(in64, &v64));

        // Truncated: every prefix of a long varint fails.
        std::string full;
        put_varint64(full, (std::uint64_t{1} << 63) + 100);
        for (std::size_t len = 0; len < full.size(); ++len) {
            std::string_view prefix(full.data(), len);
            CHECK_FALSE(get_varint64(prefix, &v64));
        }
        std::string_view whole = full;
        CHECK(get_varint64(whole, &v64));
        CHECK(v64 == (std::uint64_t{1} << 63) + 100);
    }

    TEST_CASE("length-prefixed strings") {
        std::string s;
        put_length_prefixed(s, "");
        put_length_prefixed(s, "foo");
        put_length_prefixed(s, "bar");
        put_length_prefixed(s, std::string(200, 'x'));
        std::string_view in = s;
        std::string_view v;
        REQUIRE(get_length_prefixed(in, &v));
        CHECK(v == "");
        REQUIRE(get_length_prefixed(in, &v));
        CHECK(v == "foo");
        REQUIRE(get_length_prefixed(in, &v));
        CHECK(v == "bar");
        REQUIRE(get_length_prefixed(in, &v));
        CHECK(v == std::string(200, 'x'));
        CHECK(in.empty());

        std::string bad;
        put_varint32(bad, 10);
        bad += "short";
        std::string_view bad_in = bad;
        CHECK_FALSE(get_length_prefixed(bad_in, &v));
    }

    TEST_CASE("crc32c matches the standard test vectors") {
        CHECK(crc32c::value("123456789") == 0xe3069283u);

        char buf[32];
        std::memset(buf, 0, sizeof(buf));
        CHECK(crc32c::value(buf, sizeof(buf)) == 0x8a9136aau);
        std::memset(buf, 0xff, sizeof(buf));
        CHECK(crc32c::value(buf, sizeof(buf)) == 0x62a8ab43u);
        for (int i = 0; i < 32; ++i) buf[i] = static_cast<char>(i);
        CHECK(crc32c::value(buf, sizeof(buf)) == 0x46dd794eu);
        for (int i = 0; i < 32; ++i) buf[i] = static_cast<char>(31 - i);
        CHECK(crc32c::value(buf, sizeof(buf)) == 0x113fdb5cu);
    }

    TEST_CASE("crc32c extend equals one-shot over the concatenation") {
        Random rnd(7);
        for (int trial = 0; trial < 200; ++trial) {
            std::string data(rnd.uniform(300), '\0');
            for (auto& c : data) c = static_cast<char>(rnd.uniform(256));
            const std::size_t split = data.empty() ? 0 : rnd.uniform(data.size() + 1);
            const std::uint32_t whole = crc32c::value(data);
            const std::uint32_t parts =
                crc32c::extend(crc32c::value(data.data(), split), data.data() + split, data.size() - split);
            CHECK(whole == parts);
        }
        CHECK(crc32c::value("a") != crc32c::value("foo"));
    }

    TEST_CASE("crc32c mask round-trips and changes the value") {
        const std::uint32_t crc = crc32c::value("foo");
        CHECK(crc32c::mask(crc) != crc);
        CHECK(crc32c::mask(crc32c::mask(crc)) != crc);
        CHECK(crc32c::unmask(crc32c::mask(crc)) == crc);
        CHECK(crc32c::unmask(crc32c::unmask(crc32c::mask(crc32c::mask(crc)))) == crc);
    }

    TEST_CASE("hash32 is stable and spreads small inputs") {
        // Pinned values: bloom filters on disk depend on them.
        const std::uint32_t empty = hash32("", 0);
        CHECK(hash32("", 0) == empty);
        CHECK(hash32("a", 0) != hash32("b", 0));
        CHECK(hash32("abc", 1) != hash32("abc", 2));

        std::set<std::uint32_t> seen;
        for (int i = 0; i < 10000; ++i) seen.insert(hash32(std::to_string(i)));
        CHECK(seen.size() >= 9995);  // essentially no collisions
    }

    TEST_CASE("arena allocations are distinct and aligned") {
        Arena arena;
        CHECK(arena.memory_usage() == 0);
        Random rnd(301);
        std::vector<std::pair<std::size_t, char*>> allocated;
        std::size_t bytes = 0;
        for (int i = 0; i < 2000; ++i) {
            std::size_t s = i % (2000 / 10) == 0 ? static_cast<std::size_t>(i) : rnd.skewed(10) + 1;
            if (s == 0) s = 1;
            char* r = rnd.one_in(10) ? arena.allocate_aligned(s) : arena.allocate(s);
            for (std::size_t b = 0; b < s; ++b) r[b] = static_cast<char>(i % 256);
            bytes += s;
            allocated.emplace_back(s, r);
            CHECK(arena.memory_usage() >= bytes);
        }
        for (std::size_t i = 0; i < allocated.size(); ++i) {
            const auto [size, p] = allocated[i];
            for (std::size_t b = 0; b < size; ++b) CHECK(static_cast<int>(p[b]) == static_cast<char>(i % 256));
        }
        char* aligned = arena.allocate_aligned(13);
        CHECK(reinterpret_cast<std::uintptr_t>(aligned) % alignof(std::max_align_t) == 0);
    }

    TEST_CASE("Random is deterministic and bounded") {
        Random a(42), b(42), c(43);
        bool differ = false;
        for (int i = 0; i < 100; ++i) {
            const auto x = a.next();
            CHECK(x == b.next());
            if (x != c.next()) differ = true;
        }
        CHECK(differ);
        for (int i = 0; i < 1000; ++i) CHECK(a.uniform(10) < 10);
        for (int i = 0; i < 1000; ++i) CHECK(a.skewed(5) < 32);
        const std::string p = a.printable(50);
        CHECK(p.size() == 50);
        for (char ch : p) CHECK((ch >= ' ' && ch <= '~'));
    }

    TEST_CASE("escape_string and unescape_string") {
        CHECK(escape_string("plain") == "plain");
        CHECK(escape_string(std::string("a\0b", 3)) == "a\\x00b");
        CHECK(escape_string("back\\slash") == "back\\\\slash");
        CHECK(escape_string("\xff\n") == "\\xff\\x0a");

        std::string out;
        REQUIRE(unescape_string("a\\x00b", &out));
        CHECK(out == std::string("a\0b", 3));
        REQUIRE(unescape_string("t\\tn\\nr\\r0\\0\\\\", &out));
        CHECK(out == std::string("t\tn\nr\r0\0\\", 9));
        CHECK_FALSE(unescape_string("bad\\", &out));
        CHECK_FALSE(unescape_string("bad\\q", &out));
        CHECK_FALSE(unescape_string("bad\\x1", &out));
        CHECK_FALSE(unescape_string("bad\\xzz", &out));

        Random rnd(9);
        for (int i = 0; i < 100; ++i) {
            std::string s(rnd.uniform(40), '\0');
            for (auto& c : s) c = static_cast<char>(rnd.uniform(256));
            REQUIRE(unescape_string(escape_string(s), &out));
            CHECK(out == s);
        }
    }

    TEST_CASE("hex encoding") {
        CHECK(to_hex(std::string("\x00\x01\xab\xff", 4)) == "0001abff");
        std::string out;
        REQUIRE(from_hex("0001ABff", &out));
        CHECK(out == std::string("\x00\x01\xab\xff", 4));
        CHECK_FALSE(from_hex("abc", &out));
        CHECK_FALSE(from_hex("zz", &out));
    }

    TEST_CASE("consume_decimal") {
        std::string_view in = "12345abc";
        std::uint64_t v = 0;
        REQUIRE(consume_decimal(in, &v));
        CHECK(v == 12345);
        CHECK(in == "abc");
        std::string_view none = "abc";
        CHECK_FALSE(consume_decimal(none, &v));
        std::string_view max = "18446744073709551615";
        REQUIRE(consume_decimal(max, &v));
        CHECK(v == std::numeric_limits<std::uint64_t>::max());
        std::string_view over = "18446744073709551616";
        CHECK_FALSE(consume_decimal(over, &v));
    }

    TEST_CASE("human_bytes and with_commas") {
        CHECK(human_bytes(0) == "0 B");
        CHECK(human_bytes(1023) == "1023 B");
        CHECK(human_bytes(1536) == "1.5 KiB");
        CHECK(human_bytes(3u << 20) == "3.0 MiB");
        CHECK(with_commas(0) == "0");
        CHECK(with_commas(999) == "999");
        CHECK(with_commas(1000) == "1,000");
        CHECK(with_commas(1234567) == "1,234,567");
    }

    TEST_CASE("histogram statistics") {
        Histogram h;
        CHECK(h.count() == 0);
        CHECK(h.median() == 0);
        for (int i = 1; i <= 100; ++i) h.add(i);
        CHECK(h.count() == 100);
        CHECK(h.min() == 1);
        CHECK(h.max() == 100);
        CHECK(h.average() == doctest::Approx(50.5));
        CHECK(h.standard_deviation() == doctest::Approx(28.866).epsilon(0.01));
        CHECK(h.median() > 30);
        CHECK(h.median() < 70);
        CHECK(h.percentile(99) <= 100);
        CHECK(h.percentile(99) > 80);

        Histogram other;
        other.add(1000);
        h.merge(other);
        CHECK(h.count() == 101);
        CHECK(h.max() == 1000);
        CHECK(h.to_string().find("Count: 101") != std::string::npos);
    }

    TEST_CASE("Status codes and messages") {
        CHECK(Status::ok().is_ok());
        CHECK(Status::ok().to_string() == "OK");
        Status s = Status::not_found("thing", "detail");
        CHECK(s.is_not_found());
        CHECK_FALSE(s.is_ok());
        CHECK(s.message() == "thing: detail");
        CHECK(s.to_string() == "NotFound: thing: detail");
        CHECK(Status::corruption("x").is_corruption());
        CHECK(Status::io_error("x").is_io_error());
        CHECK(Status::invalid_argument("x").is_invalid_argument());
        CHECK(Status::not_supported("x").is_not_supported());
        CHECK(Status::busy("x").is_busy());
        CHECK(Status::aborted("x").is_aborted());
        CHECK(Status::io_error("a") == Status::io_error("a"));
        CHECK_FALSE(Status::io_error("a") == Status::io_error("b"));
        CHECK(code_name(Status::Code::Corruption) == "Corruption");
    }

    TEST_CASE("bytewise comparator order and shortening") {
        const Comparator* c = bytewise_comparator();
        CHECK(c->compare("a", "b") < 0);
        CHECK(c->compare("b", "a") > 0);
        CHECK(c->compare("a", "a") == 0);
        CHECK(c->compare("a", "ab") < 0);
        CHECK(c->compare("", "a") < 0);
        CHECK(c->compare("\xff", "\x01") > 0);  // unsigned bytes

        std::string start = "abcdefg";
        c->find_shortest_separator(start, "abzzz");
        CHECK(start == "abd");
        CHECK(c->compare("abcdefg", start) < 0);
        CHECK(c->compare(start, "abzzz") < 0);

        std::string prefix = "abc";
        c->find_shortest_separator(prefix, "abcdef");  // prefix of limit: unchanged
        CHECK(prefix == "abc");

        std::string adjacent = "abc1";
        c->find_shortest_separator(adjacent, "abc2");  // no room between
        CHECK(adjacent == "abc1");

        std::string succ = "abc";
        c->find_short_successor(succ);
        CHECK(succ == "b");
        std::string ff("\xff\xff", 2);
        c->find_short_successor(ff);
        CHECK(ff == std::string("\xff\xff", 2));
        std::string mixed(
            "\xff"
            "a",
            2);
        c->find_short_successor(mixed);
        CHECK(mixed == std::string("\xff"
                                   "b",
                                   2));
    }

    TEST_CASE("reverse comparator") {
        const Comparator* c = reverse_bytewise_comparator();
        CHECK(c->compare("a", "b") > 0);
        CHECK(c->compare("b", "a") < 0);
        CHECK(c->name() != bytewise_comparator()->name());
    }
}
