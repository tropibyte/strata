// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// strata_fuzz: mutation fuzzing of every decoder that reads bytes from disk.
//
// Each target starts from valid encoded data, damages it at random (bit
// flips, byte changes, truncation, duplication, splices) and feeds it to the
// decoder. Errors are expected; crashes, hangs and sanitizer reports are
// bugs. Build with -DSTRATA_SANITIZE=ON to catch memory errors.
//
//   strata_fuzz [--target NAME|all] [--cases N] [--seed S]
//
// A failure prints the target, seed and case number; rerun with the same
// --seed and --cases to reproduce it.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "db/dbformat.hpp"
#include "db/filename.hpp"
#include "db/log_reader.hpp"
#include "db/log_writer.hpp"
#include "db/memtable.hpp"
#include "db/version_edit.hpp"
#include "db/write_batch_internal.hpp"
#include "env/mem_env.hpp"
#include "strata/comparator.hpp"
#include "strata/db.hpp"
#include "table/block.hpp"
#include "table/block_builder.hpp"
#include "table/table.hpp"
#include "table/table_builder.hpp"
#include "util/lz.hpp"
#include "util/random.hpp"
#include "util/strutil.hpp"

namespace {

using namespace strata;

// Damages `data` in one of several ways.
std::string mutate(Random& rnd, std::string data) {
    const int rounds = 1 + static_cast<int>(rnd.uniform(3));
    for (int r = 0; r < rounds; ++r) {
        if (data.empty()) {
            data.push_back(static_cast<char>(rnd.uniform(256)));
            continue;
        }
        const std::size_t pos = rnd.uniform(data.size());
        switch (rnd.uniform(7)) {
            case 0:
                data[pos] = static_cast<char>(data[pos] ^ (1 << rnd.uniform(8)));
                break;
            case 1:
                data[pos] = static_cast<char>(rnd.uniform(256));
                break;
            case 2:
                data.resize(pos);
                break;
            case 3:
                data.insert(pos, 1, static_cast<char>(rnd.uniform(256)));
                break;
            case 4:
                data.erase(pos, 1 + rnd.uniform(16));
                break;
            case 5: {
                const std::size_t len = std::min<std::size_t>(data.size() - pos, 1 + rnd.uniform(64));
                data.insert(rnd.uniform(data.size() + 1), data.substr(pos, len));
                break;
            }
            default: {
                // A run of 0x00 or 0xff, as from a zeroed or erased sector.
                const std::size_t len = std::min<std::size_t>(data.size() - pos, 1 + rnd.uniform(32));
                for (std::size_t i = 0; i < len; ++i) data[pos + i] = rnd.one_in(2) ? '\0' : '\xff';
                break;
            }
        }
    }
    return data;
}

std::string sample_value(Random& rnd) {
    if (rnd.one_in(3)) return std::string(rnd.uniform(200), static_cast<char>('a' + rnd.uniform(26)));
    return rnd.printable(rnd.uniform(60));
}

// Walks an iterator a little in every direction.
void exercise(Iterator& it, Random& rnd) {
    int steps = 0;
    for (it.seek_to_first(); it.valid() && steps < 200; it.next(), ++steps) (void)it.value();
    steps = 0;
    for (it.seek_to_last(); it.valid() && steps < 200; it.prev(), ++steps) (void)it.key();
    for (int i = 0; i < 5; ++i) {
        it.seek(rnd.printable(rnd.uniform(12)));
        if (it.valid()) {
            it.next();
            if (it.valid()) it.prev();
        }
    }
    (void)it.status();
}

struct Target {
    std::function<std::string(Random&)> make_seed;  // a valid input
    std::function<void(const std::string&, Random&)> consume;
};

// ---- Targets --------------------------------------------------------------

std::string lz_seed(Random& rnd) {
    std::string input;
    while (input.size() < 2000) input += sample_value(rnd);
    std::string out;
    lz::compress(input, out);
    return out;
}

void lz_consume(const std::string& data, Random&) {
    std::string out;
    std::size_t len = 0;
    (void)lz::uncompressed_length(data, &len);
    (void)lz::decompress(data, out);
}

std::string block_seed(Random& rnd) {
    std::map<std::string, std::string> kv;
    for (int i = 0; i < 60; ++i) kv[rnd.printable(1 + rnd.uniform(12))] = sample_value(rnd);
    BlockBuilder b(1 + static_cast<int>(rnd.uniform(8)), bytewise_comparator());
    for (const auto& [k, v] : kv) b.add(k, v);
    return std::string(b.finish());
}

void block_consume(const std::string& data, Random& rnd) {
    auto block = std::make_shared<const Block>(data);
    auto it = Block::new_iterator(block, bytewise_comparator());
    exercise(*it, rnd);
}

TableOptions fuzz_table_options() {
    TableOptions o;
    o.comparator = bytewise_comparator();
    o.block_size = 256;
    return o;
}

std::string table_seed(Random& rnd) {
    MemEnv env;
    (void)env.create_dir("/f");
    std::unique_ptr<WritableFile> file;
    (void)env.new_writable_file("/f/t.sst", &file);
    TableOptions o = fuzz_table_options();
    o.compression = rnd.one_in(2) ? Compression::Lz : Compression::None;
    TableBuilder builder(o, file.get());
    std::map<std::string, std::string> kv;
    for (int i = 0; i < 150; ++i) kv[rnd.printable(1 + rnd.uniform(12))] = sample_value(rnd);
    for (const auto& [k, v] : kv) builder.add(k, v);
    (void)builder.finish();
    std::string raw;
    env.read_raw("/f/t.sst", &raw);
    return raw;
}

void table_consume(const std::string& data, Random& rnd) {
    MemEnv env;
    (void)env.create_dir("/f");
    (void)write_string_to_file(&env, data, "/f/t.sst", false);
    std::unique_ptr<RandomAccessFile> file;
    if (!env.new_random_access_file("/f/t.sst", &file).is_ok()) return;
    BlockCache cache(1 << 16);
    std::shared_ptr<Table> table;
    if (!Table::open(fuzz_table_options(), std::move(file), data.size(), &cache, &table).is_ok()) return;
    ReadOptions ro;
    ro.verify_checksums = rnd.one_in(2);
    auto it = table->new_iterator(ro);
    exercise(*it, rnd);
    for (int i = 0; i < 5; ++i) {
        (void)table->internal_get(ro, rnd.printable(1 + rnd.uniform(12)), [](std::string_view, std::string_view) {});
        (void)table->approximate_offset_of(rnd.printable(4));
    }
}

std::string log_seed(Random& rnd) {
    MemEnv env;
    (void)env.create_dir("/f");
    std::unique_ptr<WritableFile> file;
    (void)env.new_writable_file("/f/l.log", &file);
    log::Writer writer(file.get());
    for (int i = 0; i < 20; ++i) {
        // Some records large enough to be split across blocks.
        const std::size_t n = rnd.one_in(4) ? 20000 + rnd.uniform(30000) : rnd.uniform(300);
        (void)writer.add_record(std::string(n, static_cast<char>('a' + i)));
    }
    std::string raw;
    env.read_raw("/f/l.log", &raw);
    return raw;
}

void log_consume(const std::string& data, Random&) {
    MemEnv env;
    (void)env.create_dir("/f");
    (void)write_string_to_file(&env, data, "/f/l.log", false);
    std::unique_ptr<SequentialFile> file;
    if (!env.new_sequential_file("/f/l.log", &file).is_ok()) return;
    struct Quiet final : public log::Reader::Reporter {
        void corruption(std::size_t, const Status&) override {}
    } reporter;
    log::Reader reader(file.get(), &reporter, true);
    std::string record;
    int records = 0;
    while (reader.read_record(&record)) {
        if (++records > 100000) std::abort();  // would be a hang
    }
}

std::string edit_seed(Random& rnd) {
    VersionEdit edit;
    edit.set_comparator_name("strata.BytewiseComparator");
    edit.set_log_number(rnd.uniform(1000));
    edit.set_next_file(rnd.uniform(1000) + 1000);
    edit.set_last_sequence(rnd.next() >> 8);
    for (int i = 0; i < 5; ++i) {
        edit.add_file(static_cast<int>(rnd.uniform(7)), rnd.uniform(500), rnd.uniform(1 << 20),
                      InternalKey(rnd.printable(5), rnd.uniform(1000), ValueType::Value),
                      InternalKey(rnd.printable(5), rnd.uniform(1000), ValueType::Deletion));
        edit.remove_file(static_cast<int>(rnd.uniform(7)), rnd.uniform(500));
    }
    edit.set_compact_pointer(1, InternalKey("cp", 5, ValueType::Value));
    std::string out;
    edit.encode_to(out);
    return out;
}

void edit_consume(const std::string& data, Random&) {
    VersionEdit edit;
    if (edit.decode_from(data).is_ok()) {
        std::string again;
        edit.encode_to(again);
        (void)edit.debug_string();
    }
}

std::string batch_seed(Random& rnd) {
    WriteBatch batch;
    for (int i = 0; i < 30; ++i) {
        if (rnd.one_in(3)) {
            batch.remove(rnd.printable(1 + rnd.uniform(10)));
        } else {
            batch.put(rnd.printable(1 + rnd.uniform(10)), sample_value(rnd));
        }
    }
    WriteBatchInternal::set_sequence(batch, 1000);
    return std::string(WriteBatchInternal::contents(batch));
}

void batch_consume(const std::string& data, Random&) {
    if (data.size() < WriteBatchInternal::kHeaderSize) return;
    WriteBatch batch;
    WriteBatchInternal::set_contents(batch, data);
    InternalKeyComparator icmp(bytewise_comparator());
    auto mem = std::make_shared<MemTable>(icmp);
    (void)WriteBatchInternal::insert_into(batch, mem.get());
}

// Whole-database target: build a small database, damage one of its files,
// then open it and read everything. Opening may fail; it must not crash.
struct DbImage {
    std::map<std::string, std::string> files;  // name -> contents
};

DbImage db_seed_image(Random& rnd) {
    MemEnv env;
    Options o;
    o.env = &env;
    o.create_if_missing = true;
    o.write_buffer_size = 8 * 1024;
    o.block_size = 512;
    {
        std::unique_ptr<DB> db;
        if (!DB::open(o, "/db", &db).is_ok()) std::abort();
        for (int i = 0; i < 400; ++i) {
            const std::string k = "key" + std::to_string(rnd.uniform(200));
            if (rnd.one_in(5)) {
                (void)db->remove({}, k);
            } else {
                (void)db->put({}, k, sample_value(rnd));
            }
        }
        (void)db->flush();
        for (int i = 0; i < 50; ++i) (void)db->put({}, "tail" + std::to_string(i), "in the log");
    }
    DbImage image;
    std::vector<std::string> names;
    (void)env.get_children("/db", &names);
    for (const auto& n : names) {
        if (n == "LOCK" || n.rfind("LOG", 0) == 0) continue;
        std::string data;
        env.read_raw("/db/" + n, &data);
        image.files[n] = data;
    }
    return image;
}

void db_consume_image(const DbImage& image, Random& rnd) {
    MemEnv env;
    (void)env.create_dir("/db");
    // Damage one file (and occasionally delete one).
    const std::size_t victim = rnd.uniform(image.files.size());
    std::size_t i = 0;
    for (const auto& [name, data] : image.files) {
        if (i++ == victim) {
            if (rnd.one_in(10)) continue;
            (void)write_string_to_file(&env, mutate(rnd, data), "/db/" + name, false);
        } else {
            (void)write_string_to_file(&env, data, "/db/" + name, false);
        }
    }
    Options o;
    o.env = &env;
    o.paranoid_checks = rnd.one_in(2);
    std::unique_ptr<DB> db;
    if (!DB::open(o, "/db", &db).is_ok()) return;
    ReadOptions ro;
    ro.verify_checksums = rnd.one_in(2);
    auto it = db->new_iterator(ro);
    exercise(*it, rnd);
    it.reset();
    std::string value;
    for (int k = 0; k < 20; ++k) (void)db->get(ro, "key" + std::to_string(rnd.uniform(200)), &value);
    (void)db->put({}, "after", "damage");
    (void)db->compact_range(std::nullopt, std::nullopt);
}

int usage() {
    std::fprintf(stderr, "usage: strata_fuzz [--target lz|block|table|log|edit|batch|db|all] [--cases N] [--seed S]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string target = "all";
    std::uint64_t cases = 2000;
    std::uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--target" && i + 1 < argc) {
            target = argv[++i];
        } else if (a == "--cases" && i + 1 < argc) {
            cases = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else {
            return usage();
        }
    }

    std::map<std::string, Target> targets = {
        {"lz", {lz_seed, lz_consume}},          {"block", {block_seed, block_consume}},
        {"table", {table_seed, table_consume}}, {"log", {log_seed, log_consume}},
        {"edit", {edit_seed, edit_consume}},    {"batch", {batch_seed, batch_consume}},
    };
    if (target != "all" && target != "db" && targets.count(target) == 0) return usage();

    for (const auto& [name, t] : targets) {
        if (target != "all" && target != name) continue;
        Random rnd(seed);
        std::vector<std::string> seeds;
        for (int s = 0; s < 8; ++s) seeds.push_back(t.make_seed(rnd));
        for (std::uint64_t c = 0; c < cases; ++c) {
            const std::string input = mutate(rnd, seeds[rnd.uniform(seeds.size())]);
            t.consume(input, rnd);
        }
        std::printf("%-6s %llu cases ok (seed %llu)\n", name.c_str(), static_cast<unsigned long long>(cases),
                    static_cast<unsigned long long>(seed));
    }

    if (target == "all" || target == "db") {
        Random rnd(seed);
        const DbImage image = db_seed_image(rnd);
        const std::uint64_t db_cases = std::max<std::uint64_t>(1, cases / 20);
        for (std::uint64_t c = 0; c < db_cases; ++c) db_consume_image(image, rnd);
        std::printf("%-6s %llu cases ok (seed %llu)\n", "db", static_cast<unsigned long long>(db_cases),
                    static_cast<unsigned long long>(seed));
    }
    return 0;
}
