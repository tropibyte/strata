// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Many threads at once: group commit, readers racing writers and
// compactions, snapshot consistency. Run under ThreadSanitizer in CI.

#include <atomic>
#include <thread>

#include "doctest.h"
#include "support/test_util.hpp"

using namespace strata;
using namespace strata::test;

TEST_SUITE("db-concurrency") {
    TEST_CASE("concurrent writers each see their own writes, and nothing is lost") {
        MemDB db;
        constexpr int kThreads = 4;
        constexpr int kPerThread = 2000;
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    const std::string k = "t" + std::to_string(t) + "/" + key(static_cast<std::uint64_t>(i));
                    WriteOptions w;
                    w.sync = (i % 100 == 0);
                    if (!db->put(w, k, value_for(static_cast<std::uint64_t>(i))).is_ok()) errors.fetch_add(1);
                    if (i % 10 == 0 && get(db.db(), k) != value_for(static_cast<std::uint64_t>(i))) errors.fetch_add(1);
                }
            });
        }
        for (auto& th : threads) th.join();
        CHECK(errors.load() == 0);
        REQUIRE(impl(db.db())->test_wait_for_background().is_ok());
        const auto data = contents(db.db());
        CHECK(data.size() == static_cast<std::size_t>(kThreads * kPerThread));
        std::string seq;
        REQUIRE(db->get_property("strata.last-sequence", &seq));
        CHECK(seq == std::to_string(kThreads * kPerThread));
    }

    TEST_CASE("readers never see a value go backwards") {
        // Each writer bumps a counter stored under its own key. A reader
        // must never observe a counter smaller than one it saw earlier.
        MemDB db;
        constexpr int kWriters = 3;
        std::atomic<bool> stop{false};
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kWriters; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 1; i <= 3000; ++i) {
                    // Pad with junk keys so flushes and compactions happen.
                    WriteBatch batch;
                    batch.put("counter" + std::to_string(t), std::to_string(i));
                    batch.put("junk" + std::to_string(t) + "/" + std::to_string(i % 500), std::string(100, 'j'));
                    if (!db->write({}, &batch).is_ok()) errors.fetch_add(1);
                }
            });
        }
        std::vector<std::thread> readers;
        for (int r = 0; r < 2; ++r) {
            readers.emplace_back([&] {
                std::vector<long> last(kWriters, 0);
                while (!stop.load()) {
                    for (int t = 0; t < kWriters; ++t) {
                        const std::string v = get(db.db(), "counter" + std::to_string(t));
                        if (v == "NOT_FOUND") continue;
                        const long n = std::stol(v);
                        if (n < last[static_cast<std::size_t>(t)]) errors.fetch_add(1);
                        last[static_cast<std::size_t>(t)] = n;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        stop.store(true);
        for (auto& th : readers) th.join();
        CHECK(errors.load() == 0);
        for (int t = 0; t < kWriters; ++t) CHECK(get(db.db(), "counter" + std::to_string(t)) == "3000");
    }

    TEST_CASE("a snapshot is consistent while writers change everything") {
        // Writers always set "a" and "b" to the same value in one batch; any
        // snapshot or iterator must see them equal.
        MemDB db;
        std::atomic<bool> stop{false};
        std::atomic<int> errors{0};
        std::thread writer([&] {
            for (int i = 0; i < 5000; ++i) {
                WriteBatch batch;
                const std::string v = std::to_string(i) + std::string(50, 'x');
                batch.put("a", v);
                batch.put("b", v);
                batch.put("filler/" + std::to_string(i % 300), std::string(80, 'f'));
                if (!db->write({}, &batch).is_ok()) errors.fetch_add(1);
            }
            stop.store(true);
        });
        std::thread reader([&] {
            while (!stop.load()) {
                const Snapshot* s = db->get_snapshot();
                const std::string a = get(db.db(), "a", s);
                const std::string b = get(db.db(), "b", s);
                if (a != b) errors.fetch_add(1);
                db->release_snapshot(s);

                auto it = db->new_iterator({});
                it->seek("a");
                std::string ia, ib;
                if (it->valid() && it->key() == "a") {
                    ia.assign(it->value());
                    it->next();
                    if (it->valid() && it->key() == "b") ib.assign(it->value());
                    if (ia != ib) errors.fetch_add(1);
                }
            }
        });
        writer.join();
        reader.join();
        CHECK(errors.load() == 0);
    }

    TEST_CASE("iterators and compactions run concurrently") {
        MemDB db;
        for (int i = 0; i < 2000; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        std::atomic<bool> stop{false};
        std::atomic<int> errors{0};
        std::thread compactor([&] {
            for (int round = 0; round < 5; ++round) {
                for (int i = 0; i < 2000; i += 2) {
                    if (!db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i)))
                             .is_ok())
                        errors.fetch_add(1);
                }
                if (!db->compact_range(std::nullopt, std::nullopt).is_ok()) errors.fetch_add(1);
            }
            stop.store(true);
        });
        std::vector<std::thread> scanners;
        for (int s = 0; s < 2; ++s) {
            scanners.emplace_back([&] {
                while (!stop.load()) {
                    auto it = db->new_iterator({});
                    int n = 0;
                    for (it->seek_to_first(); it->valid(); it->next(), ++n) {
                        if (it->key() != key(static_cast<std::uint64_t>(n)) ||
                            it->value() != value_for(static_cast<std::uint64_t>(n))) {
                            errors.fetch_add(1);
                            break;
                        }
                    }
                    if (n != 2000 || !it->status().is_ok()) errors.fetch_add(1);
                }
            });
        }
        compactor.join();
        for (auto& t : scanners) t.join();
        CHECK(errors.load() == 0);
    }

    TEST_CASE("concurrent snapshots, flushes and closes") {
        MemDB db;
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 3; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 300; ++i) {
                    const Snapshot* s = db->get_snapshot();
                    if (!db->put({}, "k" + std::to_string(t), std::to_string(i)).is_ok()) errors.fetch_add(1);
                    if (i % 50 == 0 && !db->flush().is_ok()) errors.fetch_add(1);
                    const std::string before = get(db.db(), "k" + std::to_string(t), s);
                    if (i > 0 && before != std::to_string(i - 1)) errors.fetch_add(1);
                    db->release_snapshot(s);
                }
            });
        }
        for (auto& th : threads) th.join();
        CHECK(errors.load() == 0);
        REQUIRE(db.reopen().is_ok());
        for (int t = 0; t < 3; ++t) CHECK(get(db.db(), "k" + std::to_string(t)) == "299");
    }
}
