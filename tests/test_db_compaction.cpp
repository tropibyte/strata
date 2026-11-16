// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Flushes and compactions: where files go, what gets dropped, and that no
// data is lost on the way.

#include <sstream>

#include "doctest.h"
#include "support/test_util.hpp"

using namespace strata;
using namespace strata::test;

namespace {

int total_files(DB& db) {
    const std::string levels = impl(db)->test_files_per_level();
    int total = 0;
    std::stringstream ss(levels);
    std::string part;
    while (std::getline(ss, part, ',')) total += std::stoi(part);
    return total;
}

std::uint64_t total_size(DB& db) {
    const Range all{"", "\xff\xff\xff\xff"};
    std::uint64_t size = 0;
    db.get_approximate_sizes(std::span<const Range>(&all, 1), std::span<std::uint64_t>(&size, 1));
    return size;
}

// Options for tests that place files by hand: flushes always go to level 0
// and nothing compacts automatically.
Options manual_options() {
    Options o;
    o.max_memtable_level = 0;
    o.level0_compaction_trigger = 100;
    o.level0_slowdown_trigger = 100;
    o.level0_stop_trigger = 100;
    o.max_bytes_for_level_base = 1u << 30;
    o.write_buffer_size = 1u << 20;
    return o;
}

}  // namespace

TEST_SUITE("db-compaction") {
    TEST_CASE("a flush makes a level-0 file") {
        MemDB db(std::make_unique<MemEnv>(), manual_options());
        REQUIRE(db->put({}, "a", "1").is_ok());
        REQUIRE(db->flush().is_ok());
        CHECK(impl(db.db())->test_files_per_level() == "1,0,0,0,0,0,0");
        REQUIRE(db->put({}, "b", "2").is_ok());
        REQUIRE(db->flush().is_ok());
        CHECK(impl(db.db())->test_files_per_level() == "2,0,0,0,0,0,0");
    }

    TEST_CASE("a flush that overlaps nothing is pushed below level 0") {
        Options o = manual_options();
        o.max_memtable_level = 2;
        MemDB db(std::make_unique<MemEnv>(), o);
        REQUIRE(db->put({}, "a", "1").is_ok());
        REQUIRE(db->flush().is_ok());
        CHECK(impl(db.db())->test_files_per_level() == "0,0,1,0,0,0,0");
        // Overlapping the level-2 file: stops above it.
        REQUIRE(db->put({}, "a", "2").is_ok());
        REQUIRE(db->flush().is_ok());
        CHECK(impl(db.db())->test_files_per_level() == "0,1,1,0,0,0,0");
        CHECK(get(db.db(), "a") == "2");
    }

    TEST_CASE("level-0 files are compacted once the trigger is reached") {
        Options o;
        o.max_memtable_level = 0;
        o.level0_compaction_trigger = 4;
        MemDB db(std::make_unique<MemEnv>(), o);
        for (int f = 0; f < 4; ++f) {
            for (int i = 0; i < 50; ++i) {
                REQUIRE(
                    db->put({}, key(static_cast<std::uint64_t>(i * 4 + f)), value_for(static_cast<std::uint64_t>(f)))
                        .is_ok());
            }
            REQUIRE(db->flush().is_ok());
        }
        REQUIRE(impl(db.db())->test_wait_for_background().is_ok());
        std::string l0;
        REQUIRE(db->get_property("strata.num-files-at-level0", &l0));
        CHECK(l0 == "0");
        std::string l1;
        REQUIRE(db->get_property("strata.num-files-at-level1", &l1));
        CHECK(std::stoi(l1) >= 1);
        for (int i = 0; i < 200; ++i)
            CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) == value_for(static_cast<std::uint64_t>(i % 4)));
    }

    TEST_CASE("a lone file with nothing below moves down without rewriting") {
        Options o = manual_options();
        o.level0_compaction_trigger = 1;
        o.level0_slowdown_trigger = 8;
        o.level0_stop_trigger = 12;
        MemDB db(std::make_unique<MemEnv>(), o);
        REQUIRE(db->put({}, "k", "v").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(impl(db.db())->test_wait_for_background().is_ok());
        CHECK(impl(db.db())->test_files_per_level() == "0,1,0,0,0,0,0");
        std::string log;
        db.close();
        REQUIRE(read_file_to_string(&db.env(), "/db/LOG", &log).is_ok());
        CHECK(log.find("Moved #") != std::string::npos);
    }

    TEST_CASE("compaction drops overwritten values") {
        MemDB db(std::make_unique<MemEnv>(), manual_options());
        const std::string big(1000, 'x');
        for (int i = 0; i < 200; ++i) {
            REQUIRE(db->put({}, "hot", big + std::to_string(i)).is_ok());
            if (i % 20 == 19) REQUIRE(db->flush().is_ok());
        }
        CHECK(total_size(db.db()) > 10 * 1000);
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(total_size(db.db()) < 3 * 1000);
        CHECK(get(db.db(), "hot") == big + "199");
        CHECK(total_files(db.db()) == 1);
    }

    TEST_CASE("compaction drops deleted keys entirely at the bottom") {
        MemDB db;
        for (int i = 0; i < 300; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(total_files(db.db()) > 0);
        for (int i = 0; i < 300; ++i) REQUIRE(db->remove({}, key(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(total_files(db.db()) == 0);
        CHECK(contents(db.db()).empty());
    }

    TEST_CASE("a deletion marker is kept while older data lies below") {
        MemDB db(std::make_unique<MemEnv>(), manual_options());
        DBImpl* d = impl(db.db());
        REQUIRE(db->put({}, "k", "old").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(d->test_compact_range(0, std::nullopt, std::nullopt).is_ok());
        REQUIRE(d->test_compact_range(1, std::nullopt, std::nullopt).is_ok());
        CHECK(d->test_files_per_level() == "0,0,1,0,0,0,0");

        REQUIRE(db->put({}, "a", "1").is_ok());
        REQUIRE(db->remove({}, "k").is_ok());
        REQUIRE(db->put({}, "z", "1").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(d->test_compact_range(0, std::nullopt, std::nullopt).is_ok());
        CHECK(d->test_files_per_level() == "0,1,1,0,0,0,0");
        CHECK(get(db.db(), "k") == "NOT_FOUND");  // the marker survived

        REQUIRE(d->test_compact_range(1, std::nullopt, std::nullopt).is_ok());
        CHECK(d->test_files_per_level() == "0,0,1,0,0,0,0");
        CHECK(get(db.db(), "k") == "NOT_FOUND");
        CHECK(contents(db.db()) == std::map<std::string, std::string>{{"a", "1"}, {"z", "1"}});
        REQUIRE(db.reopen().is_ok());
        CHECK(get(db.db(), "k") == "NOT_FOUND");
    }

    TEST_CASE("snapshots keep the versions they can see through compactions") {
        MemDB db;
        std::vector<const Snapshot*> snaps;
        for (int i = 0; i < 20; ++i) {
            REQUIRE(db->put({}, "k", "v" + std::to_string(i)).is_ok());
            if (i % 3 == 0) REQUIRE(db->remove({}, "k").is_ok());
            snaps.push_back(db->get_snapshot());
            if (i % 5 == 4) REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        }
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        for (int i = 0; i < 20; ++i) {
            CAPTURE(i);
            CHECK(get(db.db(), "k", snaps[static_cast<std::size_t>(i)]) ==
                  (i % 3 == 0 ? "NOT_FOUND" : "v" + std::to_string(i)));
        }
        for (auto* s : snaps) db->release_snapshot(s);
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(get(db.db(), "k") == "v19");
    }

    TEST_CASE("versions of one key are never split across output files") {
        // Snapshots pin every version of "hot"; with a tiny target file size
        // compaction would like to cut files between them, and must not.
        Options o;
        o.target_file_size = 1024;
        MemDB db(std::make_unique<MemEnv>(), o);
        std::vector<const Snapshot*> snaps;
        const std::string big(2000, 'h');
        REQUIRE(db->put({}, "a", "first").is_ok());
        for (int i = 0; i < 30; ++i) {
            REQUIRE(db->put({}, "hot", big + std::to_string(i)).is_ok());
            snaps.push_back(db->get_snapshot());
        }
        REQUIRE(db->put({}, "z", "last").is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        std::string err;
        REQUIRE(db->get_property("strata.background-error", &err));
        CHECK(err == "OK");
        for (int i = 0; i < 30; ++i) {
            CHECK(get(db.db(), "hot", snaps[static_cast<std::size_t>(i)]) == big + std::to_string(i));
        }
        for (auto* s : snaps) db->release_snapshot(s);
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(contents(db.db()).size() == 3);
    }

    TEST_CASE("manual compaction of part of the key space") {
        MemDB db(std::make_unique<MemEnv>(), manual_options());
        DBImpl* d = impl(db.db());
        // Three level-0 files with disjoint ranges.
        for (int f = 0; f < 3; ++f) {
            for (int i = 0; i < 10; ++i)
                REQUIRE(db->put({}, key(static_cast<std::uint64_t>(f * 100 + i)), "v").is_ok());
            REQUIRE(db->flush().is_ok());
        }
        CHECK(d->test_files_per_level() == "3,0,0,0,0,0,0");
        // Only the middle file overlaps this range.
        REQUIRE(d->test_compact_range(0, key(150), key(160)).is_ok());
        CHECK(d->test_files_per_level() == "3,0,0,0,0,0,0");  // nothing there
        REQUIRE(d->test_compact_range(0, key(105), key(106)).is_ok());
        CHECK(d->test_files_per_level() == "2,1,0,0,0,0,0");
        REQUIRE(d->test_compact_range(0, std::nullopt, std::nullopt).is_ok());
        CHECK(d->test_files_per_level() == "0,1,0,0,0,0,0");
        CHECK(contents(db.db()).size() == 30);
    }

    TEST_CASE("compact_range on a range compacts only what is needed") {
        MemDB db;
        for (int i = 0; i < 1000; ++i)
            REQUIRE(
                db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i), 200)).is_ok());
        REQUIRE(db->compact_range(key(0), key(100)).is_ok());
        REQUIRE(db->compact_range(key(900), std::nullopt).is_ok());
        REQUIRE(db->compact_range(std::nullopt, key(50)).is_ok());
        REQUIRE(db->compact_range(key(2000), key(3000)).is_ok());  // empty range
        for (int i = 0; i < 1000; i += 7)
            CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) == value_for(static_cast<std::uint64_t>(i), 200));
    }

    TEST_CASE("compaction outputs stay within the grandparent overlap limit") {
        Options o;
        o.target_file_size = 4096;
        o.write_buffer_size = 16 * 1024;
        o.max_bytes_for_level_base = 32 * 1024;
        MemDB db(std::make_unique<MemEnv>(), o);
        Random rnd(301);
        for (int i = 0; i < 20000; ++i) {
            REQUIRE(db->put({}, key(rnd.uniform(5000)), random_string(rnd, 50)).is_ok());
        }
        REQUIRE(impl(db.db())->test_wait_for_background().is_ok());
        // An output file is cut once it overlaps 10 target sizes of
        // grandparent data; allow one more file of slack.
        CHECK(impl(db.db())->test_max_next_level_overlapping_bytes() <= 20u * 4096);
    }

    TEST_CASE("repeated full compactions are stable") {
        MemDB db;
        for (int i = 0; i < 2000; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        const std::string levels = impl(db.db())->test_files_per_level();
        const auto data = contents(db.db());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(contents(db.db()) == data);
        CHECK(total_files(db.db()) > 1);
        CHECK(impl(db.db())->test_files_per_level() == levels);
    }

    TEST_CASE("writes keep working through many automatic compactions") {
        Options o;
        o.level0_compaction_trigger = 2;
        o.level0_slowdown_trigger = 3;
        o.level0_stop_trigger = 4;
        o.write_buffer_size = 8 * 1024;
        o.target_file_size = 8 * 1024;
        o.max_bytes_for_level_base = 32 * 1024;
        MemDB db(std::make_unique<MemEnv>(), o);
        std::map<std::string, std::string> model;
        Random rnd(9);
        for (int i = 0; i < 6000; ++i) {
            const std::string k = key(rnd.uniform(3000));
            if (rnd.one_in(5)) {
                REQUIRE(db->remove({}, k).is_ok());
                model.erase(k);
            } else {
                const std::string v = random_string(rnd, rnd.uniform(100));
                REQUIRE(db->put({}, k, v).is_ok());
                model[k] = v;
            }
        }
        REQUIRE(impl(db.db())->test_wait_for_background().is_ok());
        CHECK(contents(db.db()) == model);
        REQUIRE(db.reopen().is_ok());
        CHECK(contents(db.db()) == model);
    }
}
