// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The user-facing iterator: merging layers, hiding old versions and
// deletions, direction changes, snapshots and pinning.

#include "doctest.h"
#include "support/test_util.hpp"

using namespace strata;
using namespace strata::test;

namespace {

std::string iter_state(const Iterator& it) {
    if (!it.valid()) return "(invalid)";
    return std::string(it.key()) + "->" + std::string(it.value());
}

}  // namespace

TEST_SUITE("db-iterator") {
    TEST_CASE("iterator over an empty database") {
        MemDB db;
        auto it = db->new_iterator({});
        it->seek_to_first();
        CHECK(iter_state(*it) == "(invalid)");
        it->seek_to_last();
        CHECK(iter_state(*it) == "(invalid)");
        it->seek("foo");
        CHECK(iter_state(*it) == "(invalid)");
        CHECK(it->status().is_ok());
    }

    TEST_CASE("iterator with a single entry") {
        MemDB db;
        REQUIRE(db->put({}, "a", "va").is_ok());
        auto it = db->new_iterator({});
        it->seek_to_first();
        CHECK(iter_state(*it) == "a->va");
        it->next();
        CHECK(iter_state(*it) == "(invalid)");
        it->seek_to_first();
        it->prev();
        CHECK(iter_state(*it) == "(invalid)");
        it->seek_to_last();
        CHECK(iter_state(*it) == "a->va");
        it->prev();
        CHECK(iter_state(*it) == "(invalid)");
        it->seek("");
        CHECK(iter_state(*it) == "a->va");
        it->seek("a");
        CHECK(iter_state(*it) == "a->va");
        it->seek("b");
        CHECK(iter_state(*it) == "(invalid)");
    }

    TEST_CASE("iterator over several entries, in every layer") {
        MemDB db;
        SUBCASE("memtable only") {}
        SUBCASE("after flush") {
            REQUIRE(db->put({}, "zz-flushed", "f").is_ok());
        }
        REQUIRE(db->put({}, "a", "va").is_ok());
        REQUIRE(db->put({}, "b", "vb").is_ok());
        REQUIRE(db->put({}, "c", "vc").is_ok());
        REQUIRE(db->remove({}, "zz-flushed").is_ok());
        REQUIRE(db->flush().is_ok());

        auto it = db->new_iterator({});
        it->seek_to_first();
        CHECK(iter_state(*it) == "a->va");
        it->next();
        CHECK(iter_state(*it) == "b->vb");
        it->next();
        CHECK(iter_state(*it) == "c->vc");
        it->next();
        CHECK(iter_state(*it) == "(invalid)");
        it->seek_to_first();
        CHECK(iter_state(*it) == "a->va");
        it->prev();
        CHECK(iter_state(*it) == "(invalid)");

        it->seek_to_last();
        CHECK(iter_state(*it) == "c->vc");
        it->prev();
        CHECK(iter_state(*it) == "b->vb");
        it->prev();
        CHECK(iter_state(*it) == "a->va");
        it->prev();
        CHECK(iter_state(*it) == "(invalid)");

        it->seek("b");
        CHECK(iter_state(*it) == "b->vb");
        it->seek("bb");
        CHECK(iter_state(*it) == "c->vc");
        it->seek("z");
        CHECK(iter_state(*it) == "(invalid)");

        // Direction switches in the middle.
        it->seek("b");
        it->prev();
        CHECK(iter_state(*it) == "a->va");
        it->next();
        CHECK(iter_state(*it) == "b->vb");
        it->next();
        it->prev();
        CHECK(iter_state(*it) == "b->vb");
    }

    TEST_CASE("iterator ignores writes made after it was created") {
        MemDB db;
        REQUIRE(db->put({}, "a", "va").is_ok());
        REQUIRE(db->put({}, "b", "vb").is_ok());
        REQUIRE(db->put({}, "c", "vc").is_ok());
        auto it = db->new_iterator({});
        REQUIRE(db->put({}, "a", "va2").is_ok());
        REQUIRE(db->put({}, "a2", "new").is_ok());
        REQUIRE(db->remove({}, "b").is_ok());
        it->seek_to_first();
        CHECK(iter_state(*it) == "a->va");
        it->next();
        CHECK(iter_state(*it) == "b->vb");
        it->next();
        CHECK(iter_state(*it) == "c->vc");
        it->next();
        CHECK(iter_state(*it) == "(invalid)");
    }

    TEST_CASE("many versions of the same keys, with direction changes") {
        MemDB db;
        for (int round = 0; round < 5; ++round) {
            for (int i = 0; i < 20; ++i) {
                REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), "v" + std::to_string(round)).is_ok());
            }
            if (round % 2 == 1) REQUIRE(db->flush().is_ok());
        }
        // Delete every third key, then revive some of them.
        for (int i = 0; i < 20; i += 3) REQUIRE(db->remove({}, key(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->put({}, key(9), "revived").is_ok());

        std::map<std::string, std::string> model;
        for (int i = 0; i < 20; ++i) {
            if (i % 3 != 0) model[key(static_cast<std::uint64_t>(i))] = "v4";
        }
        model[key(9)] = "revived";
        CHECK(contents(db.db()) == model);
        CHECK(contents_reverse(db.db()) == model);

        // Zig-zag through the iterator against the model.
        auto it = db->new_iterator({});
        auto m = model.begin();
        it->seek_to_first();
        Random rnd(31);
        for (int step = 0; step < 300; ++step) {
            if (m == model.end()) {
                CHECK_FALSE(it->valid());
                it->seek_to_last();
                m = std::prev(model.end());
                continue;
            }
            REQUIRE(it->valid());
            CHECK(it->key() == m->first);
            CHECK(it->value() == m->second);
            if (rnd.one_in(2)) {
                it->next();
                ++m;
            } else if (m != model.begin()) {
                it->prev();
                --m;
            } else {
                it->prev();
                CHECK_FALSE(it->valid());
                it->seek_to_first();
            }
        }
    }

    TEST_CASE("iterator with a snapshot") {
        MemDB db;
        REQUIRE(db->put({}, "a", "1").is_ok());
        REQUIRE(db->put({}, "b", "1").is_ok());
        const Snapshot* snap = db->get_snapshot();
        REQUIRE(db->put({}, "a", "2").is_ok());
        REQUIRE(db->remove({}, "b").is_ok());
        REQUIRE(db->put({}, "c", "2").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());

        ReadOptions ro;
        ro.snapshot = snap;
        CHECK(contents(db.db(), ro) == std::map<std::string, std::string>{{"a", "1"}, {"b", "1"}});
        CHECK(contents_reverse(db.db(), ro) == std::map<std::string, std::string>{{"a", "1"}, {"b", "1"}});
        CHECK(contents(db.db()) == std::map<std::string, std::string>{{"a", "2"}, {"c", "2"}});
        db->release_snapshot(snap);
    }

    TEST_CASE("an open iterator keeps its files alive through compactions") {
        MemDB db;
        for (int i = 0; i < 500; ++i)
            REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), value_for(static_cast<std::uint64_t>(i))).is_ok());
        REQUIRE(db->flush().is_ok());
        auto it = db->new_iterator({});
        it->seek_to_first();
        REQUIRE(it->valid());

        // Rewrite and compact everything; the old files become obsolete.
        for (int i = 0; i < 500; ++i) REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), "new").is_ok());
        REQUIRE(db->compact_range(std::nullopt, std::nullopt).is_ok());

        int n = 0;
        for (; it->valid(); it->next(), ++n) CHECK(it->value() == value_for(static_cast<std::uint64_t>(n)));
        CHECK(n == 500);
        CHECK(it->status().is_ok());
        it.reset();
        CHECK(get(db.db(), key(7)) == "new");
    }

    TEST_CASE("seek lands on the first live key at or after the target") {
        MemDB db;
        for (int i = 0; i < 100; i += 10) REQUIRE(db->put({}, key(static_cast<std::uint64_t>(i)), "v").is_ok());
        REQUIRE(db->flush().is_ok());
        REQUIRE(db->remove({}, key(50)).is_ok());
        auto it = db->new_iterator({});
        it->seek(key(45));
        REQUIRE(it->valid());
        CHECK(it->key() == key(60));
        it->seek(key(50));
        REQUIRE(it->valid());
        CHECK(it->key() == key(60));
        it->prev();
        REQUIRE(it->valid());
        CHECK(it->key() == key(40));
        it->seek(key(91));
        CHECK_FALSE(it->valid());
    }

    TEST_CASE("iterating many keys in large values across many files") {
        MemDB db;
        std::map<std::string, std::string> model;
        Random rnd(77);
        for (int i = 0; i < 3000; ++i) {
            const std::string k = key(rnd.uniform(1500));
            const std::string v = random_string(rnd, rnd.uniform(300));
            REQUIRE(db->put({}, k, v).is_ok());
            model[k] = v;
        }
        REQUIRE(impl(db.db())->test_wait_for_background().is_ok());
        CHECK(contents(db.db()) == model);
        CHECK(contents_reverse(db.db()) == model);
    }
}
