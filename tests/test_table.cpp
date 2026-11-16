// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Blocks, table files, properties, and the merging and two-level iterators.

#include <map>

#include "db/dbformat.hpp"
#include "doctest.h"
#include "env/mem_env.hpp"
#include "strata/comparator.hpp"
#include "support/test_util.hpp"
#include "table/block.hpp"
#include "table/block_builder.hpp"
#include "table/format.hpp"
#include "table/merging_iterator.hpp"
#include "table/table.hpp"
#include "table/table_builder.hpp"
#include "table/two_level_iterator.hpp"
#include "util/coding.hpp"

using namespace strata;

namespace {

using KVMap = std::map<std::string, std::string>;

std::shared_ptr<const Block> build_block(const KVMap& data, int restart_interval) {
    BlockBuilder builder(restart_interval, bytewise_comparator());
    for (const auto& [k, v] : data) builder.add(k, v);
    return std::make_shared<const Block>(std::string(builder.finish()));
}

// Walks an iterator forward and backward and seeks to every key and to
// keys between them, comparing against the model.
void check_iterator(Iterator& it, const KVMap& model) {
    // Forward.
    auto m = model.begin();
    for (it.seek_to_first(); it.valid(); it.next(), ++m) {
        REQUIRE(m != model.end());
        CHECK(it.key() == m->first);
        CHECK(it.value() == m->second);
    }
    CHECK(m == model.end());
    CHECK(it.status().is_ok());

    // Backward.
    auto r = model.rbegin();
    for (it.seek_to_last(); it.valid(); it.prev(), ++r) {
        REQUIRE(r != model.rend());
        CHECK(it.key() == r->first);
    }
    CHECK(r == model.rend());

    // Seeks.
    for (const auto& [k, v] : model) {
        it.seek(k);
        REQUIRE(it.valid());
        CHECK(it.key() == k);
        // Just before k: lands on k.
        if (!k.empty() && k.back() != '\0') {
            std::string before = k;
            before.back() = static_cast<char>(before.back() - 1);
            before += "\xff\xff";
            it.seek(before);
            REQUIRE(it.valid());
            CHECK(it.key() <= k);
        }
        // Just after k: lands on the next key.
        it.seek(k + std::string(1, '\0'));
        auto next = model.upper_bound(k);
        if (next == model.end()) {
            CHECK_FALSE(it.valid());
        } else {
            REQUIRE(it.valid());
            CHECK(it.key() == next->first);
        }
    }

    // Mixed directions.
    if (model.size() >= 3) {
        it.seek_to_first();
        it.next();
        it.next();
        it.prev();
        auto second = std::next(model.begin());
        REQUIRE(it.valid());
        CHECK(it.key() == second->first);
        it.prev();
        REQUIRE(it.valid());
        CHECK(it.key() == model.begin()->first);
        it.prev();
        CHECK_FALSE(it.valid());
    }
}

KVMap random_map(Random& rnd, int n, std::size_t max_key = 20, std::size_t max_value = 100) {
    KVMap m;
    for (int i = 0; i < n; ++i) {
        std::string k = rnd.printable(1 + rnd.uniform(max_key));
        m[k] = rnd.printable(rnd.uniform(max_value + 1));
    }
    return m;
}

struct BuiltTable {
    std::shared_ptr<Table> table;
    std::uint64_t size = 0;
};

BuiltTable build_table(MemEnv& env, const std::string& path, const KVMap& data, const TableOptions& options,
                       BlockCache* cache = nullptr, const std::map<std::string, std::string>& props = {}) {
    std::unique_ptr<WritableFile> file;
    REQUIRE(env.new_writable_file(path, &file).is_ok());
    TableBuilder builder(options, file.get());
    for (const auto& [k, v] : data) builder.add(k, v);
    for (const auto& [k, v] : props) builder.set_property(k, v);
    REQUIRE(builder.finish().is_ok());
    REQUIRE(file->close().is_ok());
    CHECK(builder.num_entries() == data.size());

    BuiltTable out;
    REQUIRE(env.get_file_size(path, &out.size).is_ok());
    CHECK(out.size == builder.file_size());
    std::unique_ptr<RandomAccessFile> rfile;
    REQUIRE(env.new_random_access_file(path, &rfile).is_ok());
    Status s = Table::open(options, std::move(rfile), out.size, cache, &out.table);
    REQUIRE_MESSAGE(s.is_ok(), s.to_string());
    return out;
}

TableOptions default_table_options() {
    TableOptions o;
    o.comparator = bytewise_comparator();
    o.block_size = 256;
    return o;
}

// A two-level "index" over vectors of strings, for testing the iterator.
class VectorIterator final : public Iterator {
public:
    explicit VectorIterator(std::vector<std::pair<std::string, std::string>> data) : data_(std::move(data)) {}
    bool valid() const override { return pos_ < data_.size(); }
    void seek_to_first() override { pos_ = 0; }
    void seek_to_last() override { pos_ = data_.empty() ? 0 : data_.size() - 1; }
    void seek(std::string_view target) override {
        pos_ = 0;
        while (pos_ < data_.size() && data_[pos_].first < target) ++pos_;
    }
    void next() override { ++pos_; }
    void prev() override { pos_ = pos_ == 0 ? data_.size() : pos_ - 1; }
    std::string_view key() const override { return data_[pos_].first; }
    std::string_view value() const override { return data_[pos_].second; }
    Status status() const override { return Status::ok(); }

private:
    std::vector<std::pair<std::string, std::string>> data_;
    std::size_t pos_ = 0;
};

}  // namespace

TEST_SUITE("table") {
    TEST_CASE("empty block") {
        auto block = build_block({}, 16);
        CHECK(block->well_formed());
        auto it = Block::new_iterator(block, bytewise_comparator());
        it->seek_to_first();
        CHECK_FALSE(it->valid());
        it->seek_to_last();
        CHECK_FALSE(it->valid());
        it->seek("anything");
        CHECK_FALSE(it->valid());
        CHECK(it->status().is_ok());
    }

    TEST_CASE("block iteration with various restart intervals") {
        Random rnd(101);
        for (int interval : {1, 2, 3, 16, 1000}) {
            CAPTURE(interval);
            const KVMap model = random_map(rnd, 200);
            auto block = build_block(model, interval);
            auto it = Block::new_iterator(block, bytewise_comparator());
            check_iterator(*it, model);
        }
    }

    TEST_CASE("block prefix compression saves space") {
        KVMap model;
        for (int i = 0; i < 100; ++i) model["a/long/shared/prefix/" + test::key(static_cast<std::uint64_t>(i))] = "v";
        BlockBuilder compressed(16, bytewise_comparator());
        BlockBuilder flat(1, bytewise_comparator());
        for (const auto& [k, v] : model) {
            compressed.add(k, v);
            flat.add(k, v);
        }
        CHECK(compressed.finish().size() < flat.finish().size() / 2);
    }

    TEST_CASE("block builder size estimate and reset") {
        BlockBuilder b(16, bytewise_comparator());
        CHECK(b.empty());
        const std::size_t empty_size = b.current_size_estimate();
        b.add("k1", "v1");
        CHECK(b.num_entries() == 1);
        CHECK(b.current_size_estimate() > empty_size);
        const std::size_t estimate = b.current_size_estimate();
        CHECK(b.finish().size() == estimate);
        b.reset();
        CHECK(b.empty());
        CHECK(b.current_size_estimate() == empty_size);
    }

    TEST_CASE("malformed blocks report corruption") {
        for (std::string contents :
             {std::string(), std::string("\x01\x00", 2), std::string(4, '\xff'), std::string("\x00\x00\x00\x00", 4)}) {
            auto block = std::make_shared<const Block>(contents);
            CHECK_FALSE(block->well_formed());
            auto it = Block::new_iterator(block, bytewise_comparator());
            it->seek_to_first();
            CHECK_FALSE(it->valid());
            CHECK(it->status().is_corruption());
        }

        // Valid restart array but a garbage entry.
        std::string garbage = "\x05\x7f\x7f";  // shared=5 with no previous key
        put_fixed32(garbage, 0);
        put_fixed32(garbage, 1);
        auto block = std::make_shared<const Block>(garbage);
        auto it = Block::new_iterator(block, bytewise_comparator());
        it->seek_to_first();
        CHECK_FALSE(it->valid());
        CHECK(it->status().is_corruption());
    }

    TEST_CASE("block handle and footer encoding") {
        BlockHandle h(123456789, 4096);
        std::string enc;
        h.encode_to(enc);
        std::string_view in = enc;
        BlockHandle d;
        REQUIRE(d.decode_from(in).is_ok());
        CHECK(d.offset() == 123456789);
        CHECK(d.size() == 4096);

        Footer f;
        f.metaindex = BlockHandle(1, 2);
        f.index = BlockHandle(~std::uint64_t{0} >> 1, 77);
        std::string fenc;
        f.encode_to(fenc);
        CHECK(fenc.size() == Footer::kEncodedLength);
        Footer g;
        REQUIRE(g.decode_from(fenc).is_ok());
        CHECK(g.index.offset() == f.index.offset());
        CHECK(g.metaindex.size() == 2);

        fenc[fenc.size() - 1] ^= 1;
        CHECK(g.decode_from(fenc).is_corruption());
        CHECK(g.decode_from("short").is_corruption());
    }

    TEST_CASE("table round-trip with and without compression and filters") {
        Random rnd(2026);
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        int n = 0;
        for (Compression c : {Compression::None, Compression::Lz}) {
            for (int bloom : {0, 10}) {
                for (int entries : {0, 1, 10, 2000}) {
                    CAPTURE(static_cast<int>(c));
                    CAPTURE(bloom);
                    CAPTURE(entries);
                    TableOptions o = default_table_options();
                    o.compression = c;
                    o.bloom_bits_per_key = bloom;
                    o.block_restart_interval = 1 + static_cast<int>(rnd.uniform(16));
                    const KVMap model = random_map(rnd, entries);
                    auto built = build_table(env, "/t/" + std::to_string(n++) + ".sst", model, o);
                    CHECK(built.table->has_filter() == (bloom > 0 && !model.empty()));
                    CHECK(built.table->properties().num_entries == model.size());
                    auto it = built.table->new_iterator(ReadOptions());
                    check_iterator(*it, model);
                }
            }
        }
    }

    TEST_CASE("table compression shrinks compressible data") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        KVMap model;
        for (int i = 0; i < 2000; ++i)
            model[test::key(static_cast<std::uint64_t>(i))] = test::value_for(static_cast<std::uint64_t>(i), 200);
        TableOptions plain = default_table_options();
        plain.block_size = 4096;
        plain.compression = Compression::None;
        TableOptions lz = plain;
        lz.compression = Compression::Lz;
        auto a = build_table(env, "/t/plain.sst", model, plain);
        auto b = build_table(env, "/t/lz.sst", model, lz);
        CHECK(b.size < a.size * 3 / 4);
        CHECK(b.table->properties().compressed_blocks > 0);
        CHECK(a.table->properties().compressed_blocks == 0);
        auto it = b.table->new_iterator(ReadOptions());
        check_iterator(*it, model);
    }

    TEST_CASE("table properties are recorded") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        KVMap model{{"apple", "red"}, {"banana", "yellow"}, {"cherry", "dark red"}};
        auto built = build_table(env, "/t/p.sst", model, default_table_options(), nullptr,
                                 {{"custom.answer", "42"}, {"custom.name", "fruit"}});
        const TableProperties& p = built.table->properties();
        CHECK(p.num_entries == 3);
        CHECK(p.raw_key_size == 5 + 6 + 6);
        CHECK(p.raw_value_size == 3 + 6 + 8);
        CHECK(p.num_data_blocks == 1);
        CHECK(p.comparator == "strata.BytewiseComparator");
        CHECK(p.filter_size > 0);
        CHECK(p.index_size > 0);
        CHECK(p.user.at("custom.answer") == "42");
        CHECK(p.user.at("custom.name") == "fruit");

        TableProperties decoded;
        REQUIRE(TableProperties::decode(p.encode(), &decoded).is_ok());
        CHECK(decoded.num_entries == 3);
        CHECK(decoded.user.size() == 2);
    }

    TEST_CASE("table point lookups use the filter") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        KVMap model;
        for (int i = 0; i < 500; i += 2) model[test::key(static_cast<std::uint64_t>(i))] = "v" + std::to_string(i);
        auto built = build_table(env, "/t/g.sst", model, default_table_options());

        for (int i = 0; i < 500; ++i) {
            const std::string k = test::key(static_cast<std::uint64_t>(i));
            std::string found_key, found_value;
            bool called = false;
            REQUIRE(built.table
                        ->internal_get(ReadOptions(), k,
                                       [&](std::string_view fk, std::string_view fv) {
                                           called = true;
                                           found_key.assign(fk);
                                           found_value.assign(fv);
                                       })
                        .is_ok());
            if (i % 2 == 0) {
                REQUIRE(called);
                CHECK(found_key == k);
                CHECK(found_value == "v" + std::to_string(i));
            } else if (called) {
                // A false positive from the filter: the lookup still lands
                // on the next key, never on k.
                CHECK(found_key != k);
            }
        }
    }

    TEST_CASE("table approximate offsets grow with the key") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        KVMap model;
        for (int i = 0; i < 1000; ++i) model[test::key(static_cast<std::uint64_t>(i))] = std::string(100, 'v');
        TableOptions o = default_table_options();
        o.compression = Compression::None;
        auto built = build_table(env, "/t/o.sst", model, o);
        const auto start = built.table->approximate_offset_of(test::key(0));
        const auto middle = built.table->approximate_offset_of(test::key(500));
        const auto end = built.table->approximate_offset_of("zzz");
        CHECK(start == 0);
        CHECK(middle > 40000);
        CHECK(middle < 70000);
        CHECK(end >= 100000);
        CHECK(end <= built.size);
    }

    TEST_CASE("table reads go through the block cache") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        KVMap model;
        for (int i = 0; i < 300; ++i) model[test::key(static_cast<std::uint64_t>(i))] = std::string(50, 'x');
        BlockCache cache(1 << 20);
        auto built = build_table(env, "/t/c.sst", model, default_table_options(), &cache);
        {
            auto it = built.table->new_iterator(ReadOptions());
            for (it->seek_to_first(); it->valid(); it->next()) {
            }
        }
        const auto cached = cache.size();
        CHECK(cached == built.table->properties().num_data_blocks);
        const auto hits_before = cache.hits();
        {
            auto it = built.table->new_iterator(ReadOptions());
            for (it->seek_to_first(); it->valid(); it->next()) {
            }
        }
        CHECK(cache.hits() >= hits_before + cached);

        // fill_cache = false leaves the cache alone.
        cache.clear();
        ReadOptions no_fill;
        no_fill.fill_cache = false;
        auto it = built.table->new_iterator(no_fill);
        for (it->seek_to_first(); it->valid(); it->next()) {
        }
        CHECK(cache.size() == 0);
    }

    TEST_CASE("table detects damaged blocks") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        KVMap model;
        for (int i = 0; i < 200; ++i)
            model[test::key(static_cast<std::uint64_t>(i))] = test::value_for(static_cast<std::uint64_t>(i));
        TableOptions o = default_table_options();
        o.compression = Compression::None;
        auto built = build_table(env, "/t/d.sst", model, o);
        built.table.reset();

        std::string raw;
        REQUIRE(env.read_raw("/t/d.sst", &raw));
        raw[10] ^= 0x40;  // inside the first data block
        REQUIRE(env.write_raw("/t/d.sst", raw));

        std::unique_ptr<RandomAccessFile> rfile;
        REQUIRE(env.new_random_access_file("/t/d.sst", &rfile).is_ok());
        std::shared_ptr<Table> table;
        REQUIRE(Table::open(o, std::move(rfile), raw.size(), nullptr, &table).is_ok());
        ReadOptions verify;
        verify.verify_checksums = true;
        auto it = table->new_iterator(verify);
        it->seek_to_first();
        while (it->valid()) it->next();
        CHECK(it->status().is_corruption());
    }

    TEST_CASE("table open rejects non-tables and truncation") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        REQUIRE(write_string_to_file(&env, std::string(100, 'x'), "/t/junk.sst", false).is_ok());
        std::unique_ptr<RandomAccessFile> rfile;
        REQUIRE(env.new_random_access_file("/t/junk.sst", &rfile).is_ok());
        std::shared_ptr<Table> table;
        CHECK(Table::open(default_table_options(), std::move(rfile), 100, nullptr, &table).is_corruption());

        REQUIRE(write_string_to_file(&env, "tiny", "/t/tiny.sst", false).is_ok());
        REQUIRE(env.new_random_access_file("/t/tiny.sst", &rfile).is_ok());
        CHECK(Table::open(default_table_options(), std::move(rfile), 4, nullptr, &table).is_corruption());

        // A real table with a mismatched comparator.
        KVMap model{{"a", "1"}};
        auto built = build_table(env, "/t/r.sst", model, default_table_options());
        TableOptions reverse = default_table_options();
        reverse.comparator = reverse_bytewise_comparator();
        REQUIRE(env.new_random_access_file("/t/r.sst", &rfile).is_ok());
        CHECK(Table::open(reverse, std::move(rfile), built.size, nullptr, &table).is_invalid_argument());
    }

    TEST_CASE("table filter on internal keys covers the user key") {
        MemEnv env;
        REQUIRE(env.create_dir("/t").is_ok());
        InternalKeyComparator icmp(bytewise_comparator());
        TableOptions o = default_table_options();
        o.comparator = &icmp;
        o.filter_key = &filter_user_key;
        std::unique_ptr<WritableFile> file;
        REQUIRE(env.new_writable_file("/t/i.sst", &file).is_ok());
        TableBuilder builder(o, file.get());
        for (int i = 0; i < 100; ++i) {
            std::string ikey;
            append_internal_key(ikey, ParsedInternalKey(test::key(static_cast<std::uint64_t>(i)),
                                                        1000 - static_cast<std::uint64_t>(i), ValueType::Value));
            builder.add(ikey, "v");
        }
        REQUIRE(builder.finish().is_ok());
        REQUIRE(file->close().is_ok());
        std::uint64_t size = 0;
        REQUIRE(env.get_file_size("/t/i.sst", &size).is_ok());
        std::unique_ptr<RandomAccessFile> rfile;
        REQUIRE(env.new_random_access_file("/t/i.sst", &rfile).is_ok());
        std::shared_ptr<Table> table;
        REQUIRE(Table::open(o, std::move(rfile), size, nullptr, &table).is_ok());

        // Look up with a different sequence number: the filter is keyed on
        // the user key alone, so it must still match.
        const LookupKey lk(test::key(42), kMaxSequenceNumber);
        bool found = false;
        REQUIRE(table
                    ->internal_get(
                        ReadOptions(), lk.internal_key(),
                        [&](std::string_view k, std::string_view) { found = extract_user_key(k) == test::key(42); })
                    .is_ok());
        CHECK(found);
    }

    TEST_CASE("merging iterator merges sorted children") {
        Random rnd(55);
        for (int trial = 0; trial < 20; ++trial) {
            KVMap model;
            std::vector<std::unique_ptr<Iterator>> children;
            const int n = static_cast<int>(rnd.uniform(5));
            for (int c = 0; c < n; ++c) {
                std::vector<std::pair<std::string, std::string>> data;
                KVMap part = random_map(rnd, static_cast<int>(rnd.uniform(50)));
                for (const auto& [k, v] : part) {
                    if (model.count(k) != 0) continue;  // keys must be unique across children
                    model[k] = v;
                    data.emplace_back(k, v);
                }
                children.push_back(std::make_unique<VectorIterator>(std::move(data)));
            }
            auto merged = new_merging_iterator(bytewise_comparator(), std::move(children));
            check_iterator(*merged, model);
        }
    }

    TEST_CASE("merging iterator direction changes") {
        std::vector<std::unique_ptr<Iterator>> children;
        children.push_back(std::make_unique<VectorIterator>(
            std::vector<std::pair<std::string, std::string>>{{"a", "1"}, {"d", "4"}, {"g", "7"}}));
        children.push_back(
            std::make_unique<VectorIterator>(std::vector<std::pair<std::string, std::string>>{{"b", "2"}, {"e", "5"}}));
        children.push_back(
            std::make_unique<VectorIterator>(std::vector<std::pair<std::string, std::string>>{{"c", "3"}, {"f", "6"}}));
        auto it = new_merging_iterator(bytewise_comparator(), std::move(children));
        it->seek("d");
        CHECK(it->key() == "d");
        it->prev();
        CHECK(it->key() == "c");
        it->prev();
        CHECK(it->key() == "b");
        it->next();
        CHECK(it->key() == "c");
        it->next();
        CHECK(it->key() == "d");
        it->next();
        CHECK(it->key() == "e");
        it->prev();
        CHECK(it->key() == "d");
        it->seek_to_last();
        CHECK(it->key() == "g");
        it->prev();
        CHECK(it->key() == "f");
        it->next();
        CHECK(it->key() == "g");
        it->next();
        CHECK_FALSE(it->valid());
    }

    TEST_CASE("two-level iterator skips empty blocks and reports errors") {
        // Index: block name -> contents; some blocks are empty.
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> blocks = {
            {"b1", {{"a", "1"}, {"b", "2"}}}, {"b2", {}}, {"b3", {{"c", "3"}}}, {"b4", {}},
            {"b5", {{"d", "4"}, {"e", "5"}}},
        };
        std::vector<std::pair<std::string, std::string>> index = {
            {"b", "b1"}, {"bb", "b2"}, {"c", "b3"}, {"cc", "b4"}, {"e", "b5"}};
        auto it = new_two_level_iterator(std::make_unique<VectorIterator>(index), [&](std::string_view name) {
            return std::unique_ptr<Iterator>(std::make_unique<VectorIterator>(blocks.at(std::string(name))));
        });
        KVMap model{{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"}};
        check_iterator(*it, model);

        auto failing = new_two_level_iterator(std::make_unique<VectorIterator>(index), [](std::string_view) {
            return new_empty_iterator(Status::corruption("bad block"));
        });
        failing->seek_to_first();
        CHECK_FALSE(failing->valid());
        CHECK(failing->status().is_corruption());
    }

    TEST_CASE("empty iterator") {
        auto it = new_empty_iterator();
        it->seek_to_first();
        CHECK_FALSE(it->valid());
        CHECK(it->status().is_ok());
        auto bad = new_empty_iterator(Status::io_error("x"));
        CHECK(bad->status().is_io_error());
    }
}
