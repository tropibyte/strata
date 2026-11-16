// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Internal keys, the skiplist, the memtable and write batches.

#include <atomic>
#include <set>
#include <thread>

#include "db/dbformat.hpp"
#include "db/memtable.hpp"
#include "db/skiplist.hpp"
#include "db/write_batch_internal.hpp"
#include "doctest.h"
#include "strata/comparator.hpp"
#include "support/test_util.hpp"

using namespace strata;

namespace {

std::string ikey(std::string_view user, SequenceNumber seq, ValueType t) {
    std::string s;
    append_internal_key(s, ParsedInternalKey(user, seq, t));
    return s;
}

struct IntCompare {
    int operator()(std::uint64_t a, std::uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};

// Prints a batch as "Put(k, v)@seq Delete(k)@seq ..." via a memtable.
std::string print_contents(const WriteBatch& batch) {
    InternalKeyComparator icmp(bytewise_comparator());
    auto mem = std::make_shared<MemTable>(icmp);
    Status s = WriteBatchInternal::insert_into(batch, mem.get());
    std::string out;
    int count = 0;
    auto it = mem->new_iterator();
    for (it->seek_to_first(); it->valid(); it->next()) {
        ParsedInternalKey p;
        REQUIRE(parse_internal_key(it->key(), &p));
        if (p.type == ValueType::Value) {
            out += "Put(" + std::string(p.user_key) + ", " + std::string(it->value()) + ")";
        } else {
            out += "Delete(" + std::string(p.user_key) + ")";
        }
        out += "@" + std::to_string(p.sequence);
        ++count;
    }
    if (!s.is_ok()) {
        out += "ParseError()";
    } else if (count != static_cast<int>(WriteBatchInternal::count(batch))) {
        out += "CountMismatch()";
    }
    return out;
}

}  // namespace

TEST_SUITE("memtable") {
    TEST_CASE("internal key encode and parse") {
        for (std::string_view user : {std::string_view(""), std::string_view("k"), std::string_view("hello")}) {
            for (SequenceNumber seq :
                 {SequenceNumber{1}, SequenceNumber{2}, SequenceNumber{1} << 40, kMaxSequenceNumber}) {
                for (ValueType t : {ValueType::Value, ValueType::Deletion}) {
                    const std::string encoded = ikey(user, seq, t);
                    ParsedInternalKey p;
                    REQUIRE(parse_internal_key(encoded, &p));
                    CHECK(p.user_key == user);
                    CHECK(p.sequence == seq);
                    CHECK(p.type == t);
                    CHECK(extract_user_key(encoded) == user);
                }
            }
        }
        ParsedInternalKey p;
        CHECK_FALSE(parse_internal_key("bar", &p));
        std::string bad_type = "k";
        bad_type += std::string("\x09\x00\x00\x00\x00\x00\x00\x00", 8);
        CHECK_FALSE(parse_internal_key(bad_type, &p));
        CHECK(debug_internal_key(ikey("k", 5, ValueType::Value)) == "'k' @ 5 : put");
        CHECK(debug_internal_key(ikey("k", 6, ValueType::Deletion)) == "'k' @ 6 : del");
        CHECK(filter_user_key("abc") == "abc");
    }

    TEST_CASE("internal key order: user key, then newest first") {
        InternalKeyComparator icmp(bytewise_comparator());
        CHECK(icmp.compare(ikey("a", 5, ValueType::Value), ikey("b", 1, ValueType::Value)) < 0);
        CHECK(icmp.compare(ikey("a", 5, ValueType::Value), ikey("a", 4, ValueType::Value)) < 0);
        CHECK(icmp.compare(ikey("a", 5, ValueType::Value), ikey("a", 5, ValueType::Deletion)) < 0);
        CHECK(icmp.compare(ikey("a", 5, ValueType::Value), ikey("a", 5, ValueType::Value)) == 0);
        CHECK(icmp.name() == "strata.InternalKeyComparator");
    }

    TEST_CASE("internal key separators stay between their bounds") {
        InternalKeyComparator icmp(bytewise_comparator());
        auto shorten = [&](const std::string& s, const std::string& l) {
            std::string r = s;
            icmp.find_shortest_separator(r, l);
            return r;
        };
        // Same user key: unchanged.
        CHECK(shorten(ikey("foo", 100, ValueType::Value), ikey("foo", 99, ValueType::Value)) ==
              ikey("foo", 100, ValueType::Value));
        // Misordered user keys: unchanged.
        CHECK(shorten(ikey("foo", 100, ValueType::Value), ikey("bar", 99, ValueType::Value)) ==
              ikey("foo", 100, ValueType::Value));
        // Shortened: "foo" .. "hello" -> "g" with the maximal tag.
        CHECK(shorten(ikey("foo", 100, ValueType::Value), ikey("hello", 200, ValueType::Value)) ==
              ikey("g", kMaxSequenceNumber, kValueTypeForSeek));
        // Prefix: unchanged.
        CHECK(shorten(ikey("foo", 100, ValueType::Value), ikey("foobar", 200, ValueType::Value)) ==
              ikey("foo", 100, ValueType::Value));

        std::string succ = ikey("foo", 100, ValueType::Value);
        icmp.find_short_successor(succ);
        CHECK(succ == ikey("g", kMaxSequenceNumber, kValueTypeForSeek));
        std::string ff = ikey("\xff\xff", 100, ValueType::Value);
        icmp.find_short_successor(ff);
        CHECK(ff == ikey("\xff\xff", 100, ValueType::Value));
    }

    TEST_CASE("lookup key layout") {
        const LookupKey lk("user", 77);
        CHECK(lk.user_key() == "user");
        CHECK(lk.internal_key() == ikey("user", 77, kValueTypeForSeek));
        CHECK(lk.memtable_key().size() == 1 + lk.internal_key().size());
    }

    TEST_CASE("skiplist starts empty") {
        Arena arena;
        SkipList<std::uint64_t, IntCompare> list(IntCompare{}, &arena);
        CHECK_FALSE(list.contains(10));
        SkipList<std::uint64_t, IntCompare>::Iterator it(&list);
        CHECK_FALSE(it.valid());
        it.seek_to_first();
        CHECK_FALSE(it.valid());
        it.seek(100);
        CHECK_FALSE(it.valid());
        it.seek_to_last();
        CHECK_FALSE(it.valid());
    }

    TEST_CASE("skiplist insert, lookup and iteration") {
        constexpr int kN = 2000;
        constexpr std::uint64_t kR = 5000;
        Random rnd(1000);
        std::set<std::uint64_t> keys;
        Arena arena;
        SkipList<std::uint64_t, IntCompare> list(IntCompare{}, &arena);
        for (int i = 0; i < kN; ++i) {
            const std::uint64_t k = rnd.uniform(kR);
            if (keys.insert(k).second) list.insert(k);
        }
        for (std::uint64_t i = 0; i < kR; ++i) CHECK(list.contains(i) == (keys.count(i) == 1));

        SkipList<std::uint64_t, IntCompare>::Iterator it(&list);
        for (std::uint64_t i = 0; i < kR; i += 37) {
            it.seek(i);
            auto expected = keys.lower_bound(i);
            if (expected == keys.end()) {
                CHECK_FALSE(it.valid());
            } else {
                REQUIRE(it.valid());
                CHECK(it.key() == *expected);
            }
        }

        it.seek_to_first();
        for (auto k : keys) {
            REQUIRE(it.valid());
            CHECK(it.key() == k);
            it.next();
        }
        CHECK_FALSE(it.valid());

        it.seek_to_last();
        for (auto r = keys.rbegin(); r != keys.rend(); ++r) {
            REQUIRE(it.valid());
            CHECK(it.key() == *r);
            it.prev();
        }
        CHECK_FALSE(it.valid());
    }

    TEST_CASE("skiplist readers see a consistent list while one writer inserts") {
        // Keys are inserted in increasing order of their low bits; readers
        // check that the list stays sorted and every key they see once is
        // still there.
        Arena arena;
        SkipList<std::uint64_t, IntCompare> list(IntCompare{}, &arena);
        std::atomic<bool> done{false};
        std::atomic<int> errors{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) {
            readers.emplace_back([&] {
                while (!done.load(std::memory_order_acquire)) {
                    SkipList<std::uint64_t, IntCompare>::Iterator it(&list);
                    std::uint64_t prev = 0;
                    bool first = true;
                    int seen = 0;
                    for (it.seek_to_first(); it.valid() && seen < 2000; it.next(), ++seen) {
                        if (!first && it.key() <= prev) errors.fetch_add(1);
                        if (!list.contains(it.key())) errors.fetch_add(1);
                        prev = it.key();
                        first = false;
                    }
                }
            });
        }
        Random rnd(3);
        std::set<std::uint64_t> inserted;
        for (int i = 0; i < 20000; ++i) {
            const std::uint64_t k = rnd.next();
            if (inserted.insert(k).second) list.insert(k);
        }
        done.store(true, std::memory_order_release);
        for (auto& t : readers) t.join();
        CHECK(errors.load() == 0);
        for (auto k : inserted) CHECK(list.contains(k));
    }

    TEST_CASE("memtable get sees the newest visible entry") {
        InternalKeyComparator icmp(bytewise_comparator());
        auto mem = std::make_shared<MemTable>(icmp);
        mem->add(1, ValueType::Value, "k", "v1");
        mem->add(2, ValueType::Value, "k", "v2");
        mem->add(3, ValueType::Deletion, "k", "");
        mem->add(4, ValueType::Value, "k", "v4");
        mem->add(5, ValueType::Value, "other", "o5");
        CHECK(mem->num_entries() == 5);
        CHECK(mem->num_deletions() == 1);

        auto lookup = [&](std::string_view k, SequenceNumber seq) {
            std::string value;
            Status s;
            if (!mem->get(LookupKey(k, seq), &value, &s)) return std::string("MISSING");
            if (s.is_not_found()) return std::string("DELETED");
            return value;
        };
        CHECK(lookup("k", 0) == "MISSING");
        CHECK(lookup("k", 1) == "v1");
        CHECK(lookup("k", 2) == "v2");
        CHECK(lookup("k", 3) == "DELETED");
        CHECK(lookup("k", 100) == "v4");
        CHECK(lookup("other", 4) == "MISSING");
        CHECK(lookup("other", 5) == "o5");
        CHECK(lookup("absent", 100) == "MISSING");
        CHECK(lookup("j", 100) == "MISSING");  // seek lands on a different user key
        CHECK(mem->approximate_memory_usage() > 0);
    }

    TEST_CASE("memtable iterator yields internal keys in order") {
        InternalKeyComparator icmp(bytewise_comparator());
        auto mem = std::make_shared<MemTable>(icmp);
        mem->add(10, ValueType::Value, "b", "b10");
        mem->add(11, ValueType::Value, "a", "a11");
        mem->add(12, ValueType::Value, "b", "b12");
        mem->add(13, ValueType::Deletion, "c", "");
        auto it = mem->new_iterator();
        std::vector<std::string> seen;
        for (it->seek_to_first(); it->valid(); it->next()) seen.push_back(debug_internal_key(it->key()));
        CHECK(seen == std::vector<std::string>{"'a' @ 11 : put", "'b' @ 12 : put", "'b' @ 10 : put", "'c' @ 13 : del"});

        it->seek(ikey("b", 11, kValueTypeForSeek));
        REQUIRE(it->valid());
        CHECK(it->value() == "b10");
        it->prev();
        REQUIRE(it->valid());
        CHECK(it->value() == "b12");
        it->seek_to_last();
        CHECK(debug_internal_key(it->key()) == "'c' @ 13 : del");
        CHECK(it->value().empty());

        // The iterator keeps the memtable alive.
        auto alive = mem->new_iterator();
        mem.reset();
        alive->seek_to_first();
        CHECK(alive->valid());
    }

    TEST_CASE("write batch: empty") {
        WriteBatch batch;
        CHECK(print_contents(batch) == "");
        CHECK(batch.count() == 0);
        CHECK(batch.approximate_size() == WriteBatchInternal::kHeaderSize);
    }

    TEST_CASE("write batch: multiple updates are numbered in order") {
        WriteBatch batch;
        batch.put("foo", "bar");
        batch.remove("box");
        batch.put("baz", "boo");
        WriteBatchInternal::set_sequence(batch, 100);
        CHECK(WriteBatchInternal::sequence(batch) == 100);
        CHECK(batch.count() == 3);
        CHECK(print_contents(batch) == "Put(baz, boo)@102Delete(box)@101Put(foo, bar)@100");
    }

    TEST_CASE("write batch: damaged contents") {
        WriteBatch batch;
        batch.put("foo", "bar");
        batch.remove("box");
        WriteBatchInternal::set_sequence(batch, 200);
        const std::string_view contents = WriteBatchInternal::contents(batch);
        WriteBatchInternal::set_contents(batch, contents.substr(0, contents.size() - 1));
        CHECK(print_contents(batch) == "Put(foo, bar)@200ParseError()");

        WriteBatch bad_tag;
        bad_tag.put("k", "v");
        std::string raw(WriteBatchInternal::contents(bad_tag));
        raw[WriteBatchInternal::kHeaderSize] = 0x7f;
        WriteBatchInternal::set_contents(bad_tag, raw);
        class Nop final : public WriteBatch::Handler {
            void put(std::string_view, std::string_view) override {}
            void remove(std::string_view) override {}
        } nop;
        CHECK(bad_tag.iterate(nop).is_corruption());

        WriteBatch wrong_count;
        wrong_count.put("k", "v");
        WriteBatchInternal::set_count(wrong_count, 2);
        CHECK(wrong_count.iterate(nop).is_corruption());
    }

    TEST_CASE("write batch: append and clear") {
        WriteBatch a, b;
        WriteBatchInternal::set_sequence(a, 200);
        WriteBatchInternal::set_sequence(b, 300);
        a.append(b);
        CHECK(print_contents(a) == "");
        b.put("a", "va");
        a.append(b);
        CHECK(print_contents(a) == "Put(a, va)@200");
        b.clear();
        b.put("b", "vb");
        a.append(b);
        CHECK(print_contents(a) == "Put(a, va)@200Put(b, vb)@201");
        b.remove("foo");
        a.append(b);
        CHECK(print_contents(a) == "Put(a, va)@200Put(b, vb)@202Put(b, vb)@201Delete(foo)@203");
        a.clear();
        CHECK(a.count() == 0);
    }

    TEST_CASE("write batch: handler sees updates in order, binary-safe") {
        WriteBatch batch;
        const std::string k1("k\0ey", 4);
        const std::string v1(1000, '\xff');
        batch.put(k1, v1);
        batch.remove("gone");
        batch.put("", "");
        struct Recorder final : public WriteBatch::Handler {
            std::vector<std::string> ops;
            void put(std::string_view k, std::string_view v) override {
                ops.push_back("put:" + std::string(k) + ":" + std::to_string(v.size()));
            }
            void remove(std::string_view k) override { ops.push_back("del:" + std::string(k)); }
        } rec;
        REQUIRE(batch.iterate(rec).is_ok());
        REQUIRE(rec.ops.size() == 3);
        CHECK(rec.ops[0] == "put:" + k1 + ":1000");
        CHECK(rec.ops[1] == "del:gone");
        CHECK(rec.ops[2] == "put::0");
        CHECK(batch.approximate_size() > 1000);
    }
}
