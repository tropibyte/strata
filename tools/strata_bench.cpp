// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// strata_bench: throughput benchmarks for common workloads.
//
//   strata_bench --benchmarks=fillrandom,readrandom --num=1000000
//
// Each benchmark prints microseconds per operation and, where it applies,
// MB/s of key+value data. --histogram adds a latency histogram.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "strata/db.hpp"
#include "strata/env.hpp"
#include "util/crc32c.hpp"
#include "util/histogram.hpp"
#include "util/lz.hpp"
#include "util/random.hpp"
#include "util/strutil.hpp"

namespace {

using namespace strata;

constexpr const char* kDefaultBenchmarks =
    "fillseq,fillrandom,overwrite,readrandom,readseq,readreverse,readmissing,seekrandom,compact,readrandom,"
    "deleterandom";

constexpr const char* kUsage = R"(usage: strata_bench [--flag=value ...]

  --benchmarks=LIST    comma-separated, run in order (default: %s)
                       fillseq fillrandom fillsync overwrite readrandom readmissing
                       readhot readseq readreverse seekrandom deleteseq deleterandom
                       compact flush stats crc32c lz-compress lz-uncompress
  --num=N              number of keys (default 100000)
  --reads=N            number of reads for read benchmarks (default --num)
  --value_size=N       value size in bytes (default 100)
  --compression_ratio=F  how compressible values are, 0..1 (default 0.5)
  --batch=N            updates per write batch (default 1)
  --db=PATH            database directory (default: a temp directory)
  --use_existing_db    don't destroy the database first
  --mem                keep the database in memory (MemEnv)
  --write_buffer_size=N   --block_size=N   --bloom_bits=N
  --cache_size=N       block cache bytes
  --compression=lz|none
  --histogram          print a latency histogram per benchmark
  --seed=N             random seed (default 301)
)";

struct Flags {
    std::string benchmarks = kDefaultBenchmarks;
    std::uint64_t num = 100000;
    std::uint64_t reads = 0;
    std::size_t value_size = 100;
    double compression_ratio = 0.5;
    std::uint64_t batch = 1;
    std::string db;
    bool use_existing = false;
    bool mem = false;
    bool histogram = false;
    std::uint64_t seed = 301;
    Options options;
};

// Values that compress to about compression_ratio of their size: random
// printable chunks, each repeated to pad it out.
class ValueGenerator {
public:
    ValueGenerator(double ratio, std::uint64_t seed) {
        Random rnd(seed);
        while (data_.size() < (1u << 20)) {
            const std::size_t chunk = 100;
            const auto raw = static_cast<std::size_t>(static_cast<double>(chunk) * ratio);
            std::string piece = rnd.printable(raw < 1 ? 1 : raw);
            while (piece.size() < chunk) piece.append(piece.substr(0, chunk - piece.size()));
            data_.append(piece);
        }
    }

    std::string_view generate(std::size_t len) {
        if (pos_ + len > data_.size()) pos_ = 0;
        pos_ += len;
        return std::string_view(data_).substr(pos_ - len, len);
    }

private:
    std::string data_;
    std::size_t pos_ = 0;
};

class Benchmark {
public:
    explicit Benchmark(Flags flags)
        : flags_(std::move(flags)), values_(flags_.compression_ratio, flags_.seed), rnd_(flags_.seed) {
        if (flags_.reads == 0) flags_.reads = flags_.num;
        if (flags_.mem) {
            mem_env_ = new_mem_env();
            flags_.options.env = mem_env_.get();
            if (flags_.db.empty()) flags_.db = "/bench";
        } else if (flags_.db.empty()) {
            flags_.db = (std::filesystem::temp_directory_path() / "strata_bench").string();
        }
        flags_.options.create_if_missing = true;
    }

    int run() {
        print_header();
        if (!flags_.use_existing) {
            Status s = destroy_db(flags_.db, flags_.options);
            if (!s.is_ok()) return die("destroy", s);
        }
        if (!open()) return 1;

        std::size_t start = 0;
        const std::string& list = flags_.benchmarks;
        while (start <= list.size()) {
            std::size_t comma = list.find(',', start);
            if (comma == std::string::npos) comma = list.size();
            const std::string name = list.substr(start, comma - start);
            start = comma + 1;
            if (name.empty()) continue;
            if (!run_one(name)) return 1;
        }
        db_.reset();
        return 0;
    }

private:
    bool open() {
        db_.reset();
        Status s = DB::open(flags_.options, flags_.db, &db_);
        if (!s.is_ok()) {
            die("open", s);
            return false;
        }
        return true;
    }

    int die(const char* what, const Status& s) {
        std::fprintf(stderr, "%s: %s\n", what, s.to_string().c_str());
        return 1;
    }

    void print_header() {
        std::printf("strata %d.%d.%d\n", kVersionMajor, kVersionMinor, kVersionPatch);
        std::printf("Keys:       16 bytes each\n");
        std::printf("Values:     %llu bytes each (%.0f%% compressible)\n",
                    static_cast<unsigned long long>(flags_.value_size), 100.0 * (1.0 - flags_.compression_ratio));
        std::printf("Entries:    %s\n", with_commas(flags_.num).c_str());
        std::printf("Raw size:   %s\n", human_bytes(flags_.num * (16 + flags_.value_size)).c_str());
        std::printf("Database:   %s%s\n", flags_.db.c_str(), flags_.mem ? " (in memory)" : "");
        std::printf("------------------------------------------------\n");
    }

    static std::string make_key(std::uint64_t k) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(k));
        return buf;
    }

    // ---- Timing --------------------------------------------------------

    void start_timer() {
        hist_.clear();
        ops_ = 0;
        bytes_ = 0;
        found_ = 0;
        start_ = now();
        last_op_ = start_;
    }

    void finished_op() {
        ++ops_;
        if (flags_.histogram) {
            const double t = now();
            hist_.add(t - last_op_);
            last_op_ = t;
        }
    }

    void report(const std::string& name, const std::string& extra = {}) {
        const double elapsed = now() - start_;
        const double micros_per_op = ops_ == 0 ? 0 : elapsed / static_cast<double>(ops_);
        std::string message = extra;
        if (bytes_ > 0 && elapsed > 0) {
            char rate[64];
            std::snprintf(rate, sizeof(rate), "%6.1f MB/s",
                          (static_cast<double>(bytes_) / 1048576.0) / (elapsed / 1e6));
            message = message.empty() ? rate : std::string(rate) + "; " + message;
        }
        std::printf("%-14s : %11.3f micros/op;%s%s\n", name.c_str(), micros_per_op, message.empty() ? "" : " ",
                    message.c_str());
        if (flags_.histogram && hist_.count() > 0) std::printf("Microseconds per op:\n%s\n", hist_.to_string().c_str());
        std::fflush(stdout);
    }

    static double now() {
        using namespace std::chrono;
        return static_cast<double>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
    }

    // ---- Benchmarks ----------------------------------------------------

    bool run_one(const std::string& name) {
        start_timer();
        Status s;
        std::string extra;
        if (name == "fillseq") {
            s = write(/*sequential=*/true, flags_.num, WriteOptions());
        } else if (name == "fillrandom" || name == "overwrite") {
            s = write(false, flags_.num, WriteOptions());
        } else if (name == "fillsync") {
            WriteOptions w;
            w.sync = true;
            s = write(false, flags_.num / 100 + 1, w);
        } else if (name == "readrandom") {
            read_random(flags_.num, &extra);
        } else if (name == "readmissing") {
            read_missing();
        } else if (name == "readhot") {
            read_random(std::max<std::uint64_t>(1, flags_.num / 100), &extra);
        } else if (name == "readseq") {
            s = read_sequential(/*reverse=*/false);
        } else if (name == "readreverse") {
            s = read_sequential(true);
        } else if (name == "seekrandom") {
            s = seek_random(&extra);
        } else if (name == "deleteseq") {
            s = delete_keys(true);
        } else if (name == "deleterandom") {
            s = delete_keys(false);
        } else if (name == "compact") {
            s = db_->compact_range(std::nullopt, std::nullopt);
            ops_ = 1;
        } else if (name == "flush") {
            s = db_->flush();
            ops_ = 1;
        } else if (name == "stats") {
            std::string stats;
            db_->get_property("strata.stats", &stats);
            std::printf("%s\n", stats.c_str());
            return true;
        } else if (name == "crc32c") {
            crc32c_bench();
        } else if (name == "lz-compress") {
            lz_bench(/*decompress=*/false, &extra);
        } else if (name == "lz-uncompress") {
            lz_bench(true, &extra);
        } else {
            std::fprintf(stderr, "unknown benchmark '%s'\n", name.c_str());
            return false;
        }
        if (!s.is_ok()) {
            die(name.c_str(), s);
            return false;
        }
        report(name, extra);
        return true;
    }

    Status write(bool sequential, std::uint64_t n, const WriteOptions& options) {
        WriteBatch batch;
        for (std::uint64_t i = 0; i < n; i += flags_.batch) {
            batch.clear();
            for (std::uint64_t j = 0; j < flags_.batch && i + j < n; ++j) {
                const std::uint64_t k = sequential ? i + j : rnd_.uniform(flags_.num);
                const std::string key = make_key(k);
                const std::string_view value = values_.generate(flags_.value_size);
                batch.put(key, value);
                bytes_ += key.size() + value.size();
                finished_op();
            }
            Status s = db_->write(options, &batch);
            if (!s.is_ok()) return s;
        }
        return Status::ok();
    }

    void read_random(std::uint64_t key_space, std::string* extra) {
        std::string value;
        for (std::uint64_t i = 0; i < flags_.reads; ++i) {
            const std::string key = make_key(rnd_.uniform(key_space));
            if (db_->get(ReadOptions(), key, &value).is_ok()) ++found_;
            finished_op();
        }
        *extra = "(" + with_commas(found_) + " of " + with_commas(flags_.reads) + " found)";
    }

    void read_missing() {
        std::string value;
        for (std::uint64_t i = 0; i < flags_.reads; ++i) {
            const std::string key = make_key(rnd_.uniform(flags_.num)) + ".";
            (void)db_->get(ReadOptions(), key, &value);
            finished_op();
        }
    }

    Status read_sequential(bool reverse) {
        auto it = db_->new_iterator(ReadOptions());
        std::uint64_t i = 0;
        if (reverse) {
            for (it->seek_to_last(); it->valid() && i < flags_.reads; it->prev(), ++i) {
                bytes_ += it->key().size() + it->value().size();
                finished_op();
            }
        } else {
            for (it->seek_to_first(); it->valid() && i < flags_.reads; it->next(), ++i) {
                bytes_ += it->key().size() + it->value().size();
                finished_op();
            }
        }
        return it->status();
    }

    Status seek_random(std::string* extra) {
        auto it = db_->new_iterator(ReadOptions());
        for (std::uint64_t i = 0; i < flags_.reads; ++i) {
            const std::string key = make_key(rnd_.uniform(flags_.num));
            it->seek(key);
            if (it->valid() && it->key() == key) ++found_;
            finished_op();
        }
        *extra = "(" + with_commas(found_) + " of " + with_commas(flags_.reads) + " found)";
        return it->status();
    }

    Status delete_keys(bool sequential) {
        WriteBatch batch;
        for (std::uint64_t i = 0; i < flags_.num; i += flags_.batch) {
            batch.clear();
            for (std::uint64_t j = 0; j < flags_.batch && i + j < flags_.num; ++j) {
                batch.remove(make_key(sequential ? i + j : rnd_.uniform(flags_.num)));
                finished_op();
            }
            Status s = db_->write(WriteOptions(), &batch);
            if (!s.is_ok()) return s;
        }
        return Status::ok();
    }

    void crc32c_bench() {
        const std::string data(4096, 'x');
        std::uint32_t crc = 0;
        const std::uint64_t total = 500ull << 20;
        for (std::uint64_t done = 0; done < total; done += data.size()) {
            crc = crc32c::extend(crc, data.data(), data.size());
            bytes_ += data.size();
            finished_op();
        }
        if (crc == 0) std::printf("(crc %u)\n", crc);  // keep the loop from being optimized away
    }

    void lz_bench(bool decompress, std::string* extra) {
        const std::string input(values_.generate(64 * 1024));
        std::string compressed, output;
        lz::compress(input, compressed);
        const std::uint64_t total = 512ull << 20;
        for (std::uint64_t done = 0; done < total; done += input.size()) {
            if (decompress) {
                if (!lz::decompress(compressed, output)) {
                    *extra = "decompression failed";
                    return;
                }
            } else {
                compressed.clear();
                lz::compress(input, compressed);
            }
            bytes_ += input.size();
            finished_op();
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(output %.1f%% of input)",
                      100.0 * static_cast<double>(compressed.size()) / static_cast<double>(input.size()));
        *extra = buf;
    }

    Flags flags_;
    std::unique_ptr<Env> mem_env_;
    std::unique_ptr<DB> db_;
    ValueGenerator values_;
    Random rnd_;
    Histogram hist_;
    std::uint64_t ops_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t found_ = 0;
    double start_ = 0;
    double last_op_ = 0;
};

bool flag_value(std::string_view arg, std::string_view name, std::string_view* value) {
    if (arg.substr(0, name.size()) != name) return false;
    if (arg.size() == name.size()) {
        *value = {};
        return true;
    }
    if (arg[name.size()] != '=') return false;
    *value = arg.substr(name.size() + 1);
    return true;
}

bool to_u64(std::string_view s, std::uint64_t* out) {
    return strata::consume_decimal(s, out) && s.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Flags flags;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string_view v;
        std::uint64_t n = 0;
        bool ok = true;
        if (arg == "-h" || arg == "--help") {
            std::printf(kUsage, kDefaultBenchmarks);
            return 0;
        } else if (flag_value(arg, "--benchmarks", &v)) {
            flags.benchmarks.assign(v);
        } else if (flag_value(arg, "--num", &v)) {
            ok = to_u64(v, &flags.num);
        } else if (flag_value(arg, "--reads", &v)) {
            ok = to_u64(v, &flags.reads);
        } else if (flag_value(arg, "--value_size", &v)) {
            ok = to_u64(v, &n);
            flags.value_size = static_cast<std::size_t>(n);
        } else if (flag_value(arg, "--compression_ratio", &v)) {
            flags.compression_ratio = std::atof(std::string(v).c_str());
            ok = flags.compression_ratio > 0 && flags.compression_ratio <= 1;
        } else if (flag_value(arg, "--batch", &v)) {
            ok = to_u64(v, &flags.batch) && flags.batch > 0;
        } else if (flag_value(arg, "--db", &v)) {
            flags.db.assign(v);
        } else if (arg == "--use_existing_db") {
            flags.use_existing = true;
        } else if (arg == "--mem") {
            flags.mem = true;
        } else if (arg == "--histogram") {
            flags.histogram = true;
        } else if (flag_value(arg, "--seed", &v)) {
            ok = to_u64(v, &flags.seed);
        } else if (flag_value(arg, "--write_buffer_size", &v)) {
            ok = to_u64(v, &n);
            flags.options.write_buffer_size = static_cast<std::size_t>(n);
        } else if (flag_value(arg, "--block_size", &v)) {
            ok = to_u64(v, &n);
            flags.options.block_size = static_cast<std::size_t>(n);
        } else if (flag_value(arg, "--bloom_bits", &v)) {
            ok = to_u64(v, &n) && n <= 64;
            flags.options.bloom_bits_per_key = static_cast<int>(n);
        } else if (flag_value(arg, "--cache_size", &v)) {
            ok = to_u64(v, &n);
            flags.options.block_cache_bytes = static_cast<std::size_t>(n);
        } else if (flag_value(arg, "--compression", &v)) {
            if (v == "lz") {
                flags.options.compression = Compression::Lz;
            } else if (v == "none") {
                flags.options.compression = Compression::None;
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "strata_bench: bad flag '%s' (see --help)\n", argv[i]);
            return 2;
        }
    }
    Benchmark bench(std::move(flags));
    return bench.run();
}
