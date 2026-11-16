// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Crash recovery, damaged files and I/O errors.

#include <algorithm>

#include "db/filename.hpp"
#include "doctest.h"
#include "support/test_util.hpp"

using namespace strata;
using namespace strata::test;

namespace {

std::vector<std::string> files_of_type(MemEnv& env, const std::string& dir, FileType wanted) {
    std::vector<std::string> names, result;
    REQUIRE(env.get_children(dir, &names).is_ok());
    for (const auto& n : names) {
        std::uint64_t number = 0;
        FileType type;
        if (parse_file_name(n, &number, &type) && type == wanted) result.push_back(dir + "/" + n);
    }
    std::sort(result.begin(), result.end());
    return result;
}

Options big_buffer_options() {
    Options o;
    o.write_buffer_size = 8u << 20;  // not the default, so MemDB keeps it
    return o;
}

}  // namespace

TEST_SUITE("db-recovery") {
    TEST_CASE("synced writes survive a crash; later unsynced ones may not") {
        MemDB db(std::make_unique<MemEnv>(), big_buffer_options());
        WriteOptions sync;
        sync.sync = true;
        REQUIRE(db->put(sync, "a", "1").is_ok());
        REQUIRE(db->put({}, "b", "2").is_ok());
        REQUIRE(db->put(sync, "c", "3").is_ok());  // also makes "b" durable
        REQUIRE(db->put({}, "d", "4").is_ok());
        db.close();
        db.env().drop_unsynced_data();
        REQUIRE(db.open().is_ok());
        CHECK(get(db.db(), "a") == "1");
        CHECK(get(db.db(), "b") == "2");
        CHECK(get(db.db(), "c") == "3");
        CHECK(get(db.db(), "d") == "NOT_FOUND");
    }

    TEST_CASE("writes are replayed from the log across several reopens") {
        MemDB db;
        for (int round = 0; round < 5; ++round) {
            for (int i = 0; i < 50; ++i) {
                REQUIRE(db->put({}, key(static_cast<std::uint64_t>(round * 100 + i)),
                                value_for(static_cast<std::uint64_t>(round)))
                            .is_ok());
            }
            REQUIRE(db.reopen().is_ok());
        }
        for (int round = 0; round < 5; ++round) {
            for (int i = 0; i < 50; ++i)
                CHECK(get(db.db(), key(static_cast<std::uint64_t>(round * 100 + i))) ==
                      value_for(static_cast<std::uint64_t>(round)));
        }
        // Only the current log remains.
        CHECK(files_of_type(db.env(), "/db", FileType::Log).size() == 1);
    }

    TEST_CASE("a large log replays into several level-0 tables") {
        Options o = big_buffer_options();
        o.max_memtable_level = 0;
        o.level0_compaction_trigger = 100;
        o.level0_slowdown_trigger = 100;
        o.level0_stop_trigger = 100;
        MemDB db(std::make_unique<MemEnv>(), o);
        for (int i = 0; i < 2000; ++i)
            REQUIRE(
                db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i), 200)).is_ok());
        db.close();
        db.options().write_buffer_size = 64 * 1024;
        REQUIRE(db.open().is_ok());
        std::string l0;
        REQUIRE(db->get_property("strata.num-files-at-level0", &l0));
        CHECK(std::stoi(l0) >= 3);
        for (int i = 0; i < 2000; i += 11)
            CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) == value_for(static_cast<std::uint64_t>(i), 200));
    }

    TEST_CASE("sequence numbers continue after recovery") {
        MemDB db;
        REQUIRE(db->put({}, "a", "1").is_ok());
        REQUIRE(db->put({}, "b", "2").is_ok());
        std::string seq;
        REQUIRE(db->get_property("strata.last-sequence", &seq));
        CHECK(seq == "2");
        REQUIRE(db.reopen().is_ok());
        REQUIRE(db->get_property("strata.last-sequence", &seq));
        CHECK(seq == "2");
        REQUIRE(db->put({}, "a", "3").is_ok());
        CHECK(get(db.db(), "a") == "3");
        REQUIRE(db->flush().is_ok());
        REQUIRE(db.reopen().is_ok());
        REQUIRE(db->get_property("strata.last-sequence", &seq));
        CHECK(seq == "3");
        CHECK(get(db.db(), "a") == "3");
    }

    TEST_CASE("a damaged log record is skipped, or fails the open in paranoid mode") {
        MemDB db(std::make_unique<MemEnv>(), big_buffer_options());
        for (int i = 0; i < 100; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), std::string(1000, 'v')).is_ok());
        db.close();

        const auto logs = files_of_type(db.env(), "/db", FileType::Log);
        REQUIRE(logs.size() == 1);
        std::string raw;
        REQUIRE(db.env().read_raw(logs[0], &raw));
        REQUIRE(raw.size() > 70000);
        raw[40000] ^= 0x55;  // inside the second 32 KiB block
        REQUIRE(db.env().write_raw(logs[0], raw));

        db.options().paranoid_checks = true;
        Status s = db.open();
        CHECK(s.is_corruption());

        db.options().paranoid_checks = false;
        REQUIRE(db.open().is_ok());
        CHECK(get(db.db(), key(0)) == std::string(1000, 'v'));
        CHECK(get(db.db(), key(99)) == std::string(1000, 'v'));
        int found = 0;
        for (int i = 0; i < 100; ++i) {
            if (get(db.db(), key(static_cast<std::uint64_t>(i))) != "NOT_FOUND") ++found;
        }
        CHECK(found < 100);
        CHECK(found > 50);
    }

    TEST_CASE("a torn write at the end of the log is not an error") {
        MemDB db(std::make_unique<MemEnv>(), big_buffer_options());
        for (int i = 0; i < 10; ++i) REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), "v").is_ok());
        db.close();
        const auto logs = files_of_type(db.env(), "/db", FileType::Log);
        REQUIRE(logs.size() == 1);
        std::string raw;
        REQUIRE(db.env().read_raw(logs[0], &raw));
        raw.resize(raw.size() - 3);
        REQUIRE(db.env().write_raw(logs[0], raw));

        db.options().paranoid_checks = true;
        REQUIRE(db.open().is_ok());
        for (int i = 0; i < 9; ++i) CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) == "v");
        CHECK(get(db.db(), key(9)) == "NOT_FOUND");
    }

    TEST_CASE("a missing table file fails the open") {
        MemDB db;
        REQUIRE(db->put({}, "k", "v").is_ok());
        REQUIRE(db->flush().is_ok());
        db.close();
        const auto tables = files_of_type(db.env(), "/db", FileType::Table);
        REQUIRE(tables.size() == 1);
        REQUIRE(db.env().remove_file(tables[0]).is_ok());
        Status s = db.open();
        CHECK(s.is_corruption());
        CHECK(s.message().find("missing") != std::string::npos);
    }

    TEST_CASE("a damaged table is reported on read") {
        MemDB db;
        db.options().compression = Compression::None;
        REQUIRE(db.reopen().is_ok());
        for (int i = 0; i < 100; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->flush().is_ok());
        db.close();
        const auto tables = files_of_type(db.env(), "/db", FileType::Table);
        REQUIRE(tables.size() == 1);
        std::string raw;
        REQUIRE(db.env().read_raw(tables[0], &raw));
        raw[100] ^= 0x01;
        REQUIRE(db.env().write_raw(tables[0], raw));

        REQUIRE(db.open().is_ok());
        ReadOptions verify;
        verify.verify_checksums = true;
        auto it = db->new_iterator(verify);
        for (it->seek_to_first(); it->valid(); it->next()) {
        }
        CHECK(it->status().is_corruption());
    }

    TEST_CASE("a damaged CURRENT or MANIFEST fails the open") {
        SUBCASE("CURRENT without newline") {
            MemDB db;
            db.close();
            REQUIRE(write_string_to_file(&db.env(), "MANIFEST-000002", "/db/CURRENT", true).is_ok());
            CHECK(db.open().is_corruption());
        }
        SUBCASE("CURRENT naming a missing manifest") {
            MemDB db;
            db.close();
            REQUIRE(write_string_to_file(&db.env(), "MANIFEST-999999\n", "/db/CURRENT", true).is_ok());
            CHECK(db.open().is_corruption());
        }
        SUBCASE("garbage manifest") {
            MemDB db;
            db.close();
            const auto manifests = files_of_type(db.env(), "/db", FileType::Descriptor);
            REQUIRE(manifests.size() == 1);
            std::string raw;
            REQUIRE(db.env().read_raw(manifests[0], &raw));
            for (std::size_t i = 7; i < raw.size(); ++i) raw[i] = static_cast<char>(raw[i] ^ 0x5a);
            REQUIRE(db.env().write_raw(manifests[0], raw));
            CHECK(db.open().is_corruption());
        }
    }

    TEST_CASE("each open writes a fresh MANIFEST and removes the old one") {
        MemDB db;
        auto manifests = files_of_type(db.env(), "/db", FileType::Descriptor);
        REQUIRE(manifests.size() == 1);
        const std::string first = manifests[0];
        REQUIRE(db->put({}, "k", "v").is_ok());
        REQUIRE(db.reopen().is_ok());
        manifests = files_of_type(db.env(), "/db", FileType::Descriptor);
        REQUIRE(manifests.size() == 1);
        CHECK(manifests[0] != first);
        std::string current;
        REQUIRE(read_file_to_string(&db.env(), "/db/CURRENT", &current).is_ok());
        CHECK("/db/" + current.substr(0, current.size() - 1) == manifests[0]);
    }

    TEST_CASE("obsolete tables and logs are deleted") {
        MemDB db;
        for (int i = 0; i < 2000; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i % 500)), value_for(static_cast<std::uint64_t>(i)))
                        .is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        int live = 0;
        for (char c : impl(db.db())->test_files_per_level()) {
            if (c >= '0' && c <= '9') live += c - '0';
        }
        CHECK(files_of_type(db.env(), "/db", FileType::Table).size() == static_cast<std::size_t>(live));
        CHECK(files_of_type(db.env(), "/db", FileType::Log).size() == 1);
        CHECK(files_of_type(db.env(), "/db", FileType::Temp).empty());
    }

    TEST_CASE("a failed flush stops writes; reopening recovers the data") {
        MemDB db(std::make_unique<MemEnv>(), big_buffer_options());
        for (int i = 0; i < 100; ++i) REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), "v").is_ok());
        db.env().set_sync_error(true);
        Status s = db->flush();
        CHECK(s.is_io_error());
        CHECK(db->put({}, "after", "x").is_io_error());
        std::string err;
        REQUIRE(db->get_property("strata.background-error", &err));
        CHECK(err.find("IOError") == 0);
        CHECK(get(db.db(), key(5)) == "v");  // reads still work
        db.env().set_sync_error(false);

        REQUIRE(db.reopen().is_ok());
        for (int i = 0; i < 100; ++i) CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) == "v");
        CHECK(get(db.db(), "after") == "NOT_FOUND");
        REQUIRE(db->put({}, "after", "x").is_ok());
    }

    TEST_CASE("a failed log sync fails the write and stops further writes") {
        MemDB db;
        db.env().set_sync_error(true);
        WriteOptions sync;
        sync.sync = true;
        CHECK(db->put(sync, "k", "v").is_io_error());
        db.env().set_sync_error(false);
        CHECK(db->put({}, "k2", "v2").is_io_error());
        REQUIRE(db.reopen().is_ok());
        REQUIRE(db->put({}, "k3", "v3").is_ok());
        CHECK(get(db.db(), "k3") == "v3");
    }

    TEST_CASE("a failed log write fails only that write") {
        MemDB db;
        db.env().set_write_error(true);
        CHECK(db->put({}, "k", "v").is_io_error());
        db.env().set_write_error(false);
        REQUIRE(db->put({}, "k2", "v2").is_ok());
        CHECK(get(db.db(), "k") == "NOT_FOUND");
        CHECK(get(db.db(), "k2") == "v2");
        REQUIRE(db.reopen().is_ok());
        CHECK(get(db.db(), "k2") == "v2");
    }

    TEST_CASE("a database that cannot create its log fails to open cleanly") {
        MemDB db;
        db.close();
        db.env().set_create_error(true);
        CHECK_FALSE(db.open().is_ok());
        db.env().set_create_error(false);
        REQUIRE(db.open().is_ok());
    }

    TEST_CASE("every synced write survives repeated crashes") {
        MemDB db;
        WriteOptions sync;
        sync.sync = true;
        std::map<std::string, std::string> model;
        Random rnd(4242);
        for (int round = 0; round < 15; ++round) {
            const int ops = 50 + static_cast<int>(rnd.uniform(400));
            for (int i = 0; i < ops; ++i) {
                const std::string k = key(rnd.uniform(400));
                if (rnd.one_in(4)) {
                    REQUIRE(db->remove(sync, k).is_ok());
                    model.erase(k);
                } else {
                    const std::string v = random_string(rnd, rnd.uniform(200));
                    REQUIRE(db->put(sync, k, v).is_ok());
                    model[k] = v;
                }
            }
            if (rnd.one_in(3)) REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
            db.close();
            db.env().drop_unsynced_data();
            REQUIRE(db.open().is_ok());
            REQUIRE(contents(db.db()) == model);
        }
    }
}
