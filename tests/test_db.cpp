// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Basic database behavior: opening, reads and writes, snapshots, batches,
// properties and sizes.

#include "doctest.h"
#include "support/test_util.hpp"

using namespace strata;
using namespace strata::test;

TEST_SUITE("db") {
    TEST_CASE("open requires create_if_missing for a new database") {
        MemEnv env;
        Options options;
        options.env = &env;
        std::unique_ptr<DB> db;
        Status s = DB::open(options, "/missing", &db);
        CHECK(s.is_invalid_argument());
        CHECK(s.message().find("does not exist") != std::string::npos);
        CHECK(db == nullptr);

        options.create_if_missing = true;
        REQUIRE(DB::open(options, "/missing", &db).is_ok());
        REQUIRE(db != nullptr);
        db.reset();

        options.error_if_exists = true;
        s = DB::open(options, "/missing", &db);
        CHECK(s.is_invalid_argument());
        CHECK(s.message().find("exists") != std::string::npos);
    }

    TEST_CASE("a database can be open only once") {
        MemEnv env;
        Options options;
        options.env = &env;
        options.create_if_missing = true;
        std::unique_ptr<DB> first;
        REQUIRE(DB::open(options, "/db", &first).is_ok());
        std::unique_ptr<DB> second;
        CHECK(DB::open(options, "/db", &second).is_io_error());
        first.reset();
        CHECK(DB::open(options, "/db", &second).is_ok());
    }

    TEST_CASE("put, get and remove") {
        MemDB db;
        CHECK(get(db.db(), "foo") == "NOT_FOUND");
        REQUIRE(db->put({}, "foo", "v1").is_ok());
        CHECK(get(db.db(), "foo") == "v1");
        REQUIRE(db->put({}, "foo", "v2").is_ok());
        CHECK(get(db.db(), "foo") == "v2");
        REQUIRE(db->remove({}, "foo").is_ok());
        CHECK(get(db.db(), "foo") == "NOT_FOUND");
        REQUIRE(db->remove({}, "never-existed").is_ok());

        std::string untouched = "keep";
        CHECK(db->get({}, "foo", &untouched).is_not_found());
        CHECK(untouched == "keep");
    }

    TEST_CASE("empty keys and values, binary data and large values") {
        MemDB db;
        REQUIRE(db->put({}, "", "empty key").is_ok());
        REQUIRE(db->put({}, "empty value", "").is_ok());
        const std::string binary_key("\x00\x01\xff\x00", 4);
        const std::string binary_value("\xff\x00\x7f", 3);
        REQUIRE(db->put({}, binary_key, binary_value).is_ok());
        const std::string big(1 << 20, 'B');
        REQUIRE(db->put({}, "big", big).is_ok());

        auto check_all = [&] {
            CHECK(get(db.db(), "") == "empty key");
            CHECK(get(db.db(), "empty value") == "");
            CHECK(get(db.db(), binary_key) == binary_value);
            CHECK(get(db.db(), "big") == big);
        };
        check_all();
        REQUIRE(db->flush().is_ok());
        check_all();
        REQUIRE(db.reopen().is_ok());
        check_all();
    }

    TEST_CASE("reads find data in the memtable, level 0 and deeper levels") {
        MemDB db;
        REQUIRE(db->put({}, "a", "va").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        REQUIRE(db->put({}, "b", "vb").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->put({}, "c", "vc").is_ok());
        CHECK(get(db.db(), "a") == "va");
        CHECK(get(db.db(), "b") == "vb");
        CHECK(get(db.db(), "c") == "vc");
        CHECK(get(db.db(), "d") == "NOT_FOUND");
    }

    TEST_CASE("newer writes hide older ones across every layer") {
        MemDB db;
        REQUIRE(db->put({}, "k", "v1").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());  // v1 deep
        REQUIRE(db->put({}, "k", "v2").is_ok());
        REQUIRE(db->flush().is_ok());  // v2 in level 0 (or pushed down)
        CHECK(get(db.db(), "k") == "v2");
        REQUIRE(db->remove({}, "k").is_ok());  // tombstone in memtable
        CHECK(get(db.db(), "k") == "NOT_FOUND");
        REQUIRE(db->flush().is_ok());
        CHECK(get(db.db(), "k") == "NOT_FOUND");
        REQUIRE(db->put({}, "k", "v3").is_ok());
        CHECK(get(db.db(), "k") == "v3");
        REQUIRE(db.reopen().is_ok());
        CHECK(get(db.db(), "k") == "v3");
    }

    TEST_CASE("data survives reopen with and without a flush") {
        MemDB db;
        for (int i = 0; i < 1000; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db.reopen().is_ok());
        for (int i = 0; i < 1000; i += 37)
            CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) == value_for(static_cast<std::uint64_t>(i)));
        for (int i = 0; i < 1000; i += 2) REQUIRE(db->remove({}, key(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db.reopen().is_ok());
        REQUIRE(db.reopen().is_ok());
        for (int i = 0; i < 1000; ++i) {
            CHECK(get(db.db(), key(static_cast<std::uint64_t>(i))) ==
                  (i % 2 == 0 ? "NOT_FOUND" : value_for(static_cast<std::uint64_t>(i))));
        }
    }

    TEST_CASE("write batches apply atomically and in order") {
        MemDB db;
        REQUIRE(db->put({}, "a", "old").is_ok());
        WriteBatch batch;
        batch.put("a", "first");
        batch.remove("a");
        batch.put("a", "final");
        batch.put("b", "bee");
        batch.remove("c");
        REQUIRE(db->write({}, &batch).is_ok());
        CHECK(get(db.db(), "a") == "final");
        CHECK(get(db.db(), "b") == "bee");
        CHECK(get(db.db(), "c") == "NOT_FOUND");

        std::string seq;
        REQUIRE(db->get_property("strata.last-sequence", &seq));
        CHECK(seq == "6");  // 1 put + 5 batch updates

        WriteBatch empty;
        REQUIRE(db->write({}, &empty).is_ok());
        REQUIRE(db->get_property("strata.last-sequence", &seq));
        CHECK(seq == "6");
    }

    TEST_CASE("sync writes") {
        MemDB db;
        WriteOptions sync;
        sync.sync = true;
        const auto before = db.env().sync_count();
        REQUIRE(db->put(sync, "k", "v").is_ok());
        CHECK(db.env().sync_count() > before);
        CHECK(get(db.db(), "k") == "v");
    }

    TEST_CASE("snapshots see the state when they were taken") {
        MemDB db;
        REQUIRE(db->put({}, "k", "v1").is_ok());
        const Snapshot* s1 = db->get_snapshot();
        REQUIRE(db->put({}, "k", "v2").is_ok());
        REQUIRE(db->put({}, "only-later", "x").is_ok());
        const Snapshot* s2 = db->get_snapshot();
        REQUIRE(db->remove({}, "k").is_ok());

        CHECK(get(db.db(), "k", s1) == "v1");
        CHECK(get(db.db(), "only-later", s1) == "NOT_FOUND");
        CHECK(get(db.db(), "k", s2) == "v2");
        CHECK(get(db.db(), "k") == "NOT_FOUND");

        // Flushing and compacting must not lose what snapshots can see.
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(get(db.db(), "k", s1) == "v1");
        CHECK(get(db.db(), "k", s2) == "v2");
        CHECK(get(db.db(), "k") == "NOT_FOUND");

        ReadOptions ro;
        ro.snapshot = s1;
        CHECK(contents(db.db(), ro) == std::map<std::string, std::string>{{"k", "v1"}});
        ro.snapshot = s2;
        CHECK(contents(db.db(), ro) == std::map<std::string, std::string>{{"k", "v2"}, {"only-later", "x"}});

        CHECK(s1->sequence() < s2->sequence());
        std::string n;
        REQUIRE(db->get_property("strata.num-snapshots", &n));
        CHECK(n == "2");
        db->release_snapshot(s1);
        db->release_snapshot(s2);
        db->release_snapshot(nullptr);  // allowed
        REQUIRE(db->get_property("strata.num-snapshots", &n));
        CHECK(n == "0");

        // With the snapshots gone, a compaction drops the old versions.
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
        CHECK(contents(db.db()) == std::map<std::string, std::string>{{"only-later", "x"}});
    }

    TEST_CASE("properties") {
        MemDB db;
        std::string v;
        CHECK_FALSE(db->get_property("no-such-property", &v));
        CHECK_FALSE(db->get_property("strata.no-such-property", &v));
        CHECK_FALSE(db->get_property("strata.num-files-at-level99", &v));
        CHECK_FALSE(db->get_property("strata.num-files-at-levelx", &v));
        REQUIRE(db->get_property("strata.num-files-at-level0", &v));
        CHECK(v == "0");

        for (int i = 0; i < 100; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->flush().is_ok());
        std::string total;
        int files = 0;
        for (int level = 0; level < db.options().num_levels; ++level) {
            REQUIRE(db->get_property("strata.num-files-at-level" + std::to_string(level), &v));
            files += std::stoi(v);
        }
        CHECK(files == 1);

        REQUIRE(db->get_property("strata.stats", &v));
        CHECK(v.find("Level") != std::string::npos);
        CHECK(v.find("Block cache") != std::string::npos);
        REQUIRE(db->get_property("strata.sstables", &v));
        CHECK(v.find("--- level 0 ---") != std::string::npos);
        CHECK(v.find(key(0)) != std::string::npos);
        REQUIRE(db->get_property("strata.approximate-memory-usage", &v));
        CHECK(std::stoull(v) > 0);
        REQUIRE(db->get_property("strata.background-error", &v));
        CHECK(v == "OK");
        REQUIRE(db->get_property("strata.levels", &v));
        CHECK(v.find("files[") == 0);
    }

    TEST_CASE("approximate sizes track stored data") {
        MemDB db;
        Options& o = db.options();
        o.compression = Compression::None;
        REQUIRE(db.reopen().is_ok());

        const Range everything{"", "\xff"};
        std::uint64_t size = 0;
        db->get_approximate_sizes(std::span<const Range>(&everything, 1), std::span<std::uint64_t>(&size, 1));
        CHECK(size == 0);

        for (int i = 0; i < 2000; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), std::string(1000, 'v')).is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());

        const std::string k0 = key(0), k500 = key(500), k1000 = key(1000), k2000 = key(2000);
        const Range ranges[] = {{"", "\xff"}, {k0, k1000}, {k1000, k2000}, {k500, k500}, {"zzz", "zzzz"}};
        std::uint64_t sizes[5] = {};
        db->get_approximate_sizes(ranges, sizes);
        CHECK(sizes[0] >= 2000u * 1000);
        CHECK(sizes[0] <= 2000u * 1200);
        CHECK(sizes[1] >= 900u * 1000);
        CHECK(sizes[1] <= 1100u * 1100);
        CHECK(sizes[2] >= 900u * 1000);
        CHECK(sizes[3] == 0);
        CHECK(sizes[4] == 0);
    }

    TEST_CASE("destroy_db removes everything") {
        MemDB db;
        REQUIRE(db->put({}, "k", "v").is_ok());
        REQUIRE(db->flush().is_ok());
        CHECK_FALSE(destroy_db(db.path(), db.options()).is_ok());  // still open
        db.close();
        REQUIRE(destroy_db(db.path(), db.options()).is_ok());
        CHECK_FALSE(db.env().file_exists(db.path()));
        CHECK(destroy_db("/never-was", db.options()).is_ok());
        Options no_create = db.options();
        no_create.create_if_missing = false;
        std::unique_ptr<DB> again;
        CHECK(DB::open(no_create, db.path(), &again).is_invalid_argument());
    }

    TEST_CASE("flush of an empty memtable is a no-op") {
        MemDB db;
        REQUIRE(db->flush().is_ok());
        CHECK(impl(db.db())->test_files_per_level() == "0,0,0,0,0,0,0");
        REQUIRE(db->put({}, "x", "y").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->flush().is_ok());
        int files = 0;
        for (char c : impl(db.db())->test_files_per_level()) {
            if (c >= '1' && c <= '9') files += c - '0';
        }
        CHECK(files == 1);
    }

    TEST_CASE("an info LOG is written in the database directory") {
        MemDB db;
        REQUIRE(db->put({}, "k", "v").is_ok());
        REQUIRE(db->flush().is_ok());
        db.close();
        std::string log;
        REQUIRE(read_file_to_string(&db.env(), "/db/LOG", &log).is_ok());
        CHECK(log.find("Opened /db") != std::string::npos);
        CHECK(log.find("Level-0 table") != std::string::npos);
        REQUIRE(db.open().is_ok());
        CHECK(db.env().file_exists("/db/LOG.old"));
    }

    TEST_CASE("a custom comparator orders keys and must match on reopen") {
        auto env = std::make_unique<MemEnv>();
        MemEnv* raw_env = env.get();
        Options options;
        options.comparator = reverse_bytewise_comparator();
        MemDB db(std::move(env), options);
        for (const char* k : {"b", "a", "d", "c"}) REQUIRE(db->put({}, k, std::string("v") + k).is_ok());
        REQUIRE(db->flush().is_ok());
        std::vector<std::string> order;
        auto it = db->new_iterator({});
        for (it->seek_to_first(); it->valid(); it->next()) order.emplace_back(it->key());
        CHECK(order == std::vector<std::string>{"d", "c", "b", "a"});
        it->seek("c");
        REQUIRE(it->valid());
        CHECK(it->key() == "c");
        it.reset();
        CHECK(get(db.db(), "c") == "vc");
        db.close();

        Options wrong = db.options();
        wrong.comparator = bytewise_comparator();
        wrong.env = raw_env;
        std::unique_ptr<DB> other;
        Status s = DB::open(wrong, db.path(), &other);
        CHECK(s.is_invalid_argument());
        CHECK(s.message().find("comparator") != std::string::npos);
    }

    TEST_CASE("the local file system Env works end to end") {
        TempDir dir;
        Options options = small_options(Env::default_env());
        const std::string path = dir.file("db");
        {
            std::unique_ptr<DB> db;
            REQUIRE(DB::open(options, path, &db).is_ok());
            for (int i = 0; i < 3000; ++i)
                REQUIRE(
                    db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
            for (int i = 0; i < 3000; i += 3) REQUIRE(db->remove({}, key(static_cast<std::uint64_t>(i))).is_ok());
            REQUIRE(impl(*db)->test_wait_for_background().is_ok());
        }
        {
            std::unique_ptr<DB> db;
            REQUIRE(DB::open(options, path, &db).is_ok());
            const auto data = contents(*db);
            CHECK(data.size() == 2000);
            for (int i = 0; i < 3000; ++i) {
                CHECK(get(*db, key(static_cast<std::uint64_t>(i))) ==
                      (i % 3 == 0 ? "NOT_FOUND" : value_for(static_cast<std::uint64_t>(i))));
            }
            REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());
            CHECK(contents(*db) == data);
        }
        REQUIRE(destroy_db(path, options).is_ok());
        CHECK_FALSE(Env::default_env()->file_exists(path));
    }

    TEST_CASE("options are sanitized") {
        InternalKeyComparator icmp(bytewise_comparator());
        Options wild;
        wild.num_levels = 100;
        wild.write_buffer_size = 1;
        wild.max_open_files = 1;
        wild.level0_compaction_trigger = 0;
        wild.level0_slowdown_trigger = 0;
        wild.level0_stop_trigger = 0;
        wild.max_memtable_level = 50;
        wild.level_size_multiplier = 0.5;
        const Options o = sanitize_options(&icmp, wild);
        CHECK(o.num_levels == 20);
        CHECK(o.write_buffer_size >= 1024);
        CHECK(o.max_open_files >= 20);
        CHECK(o.level0_compaction_trigger >= 1);
        CHECK(o.level0_slowdown_trigger >= o.level0_compaction_trigger);
        CHECK(o.level0_stop_trigger >= o.level0_slowdown_trigger);
        CHECK(o.max_memtable_level == 19);
        CHECK(o.level_size_multiplier >= 2.0);
        CHECK(o.env == Env::default_env());
        CHECK(o.comparator == bytewise_comparator());
    }
}
