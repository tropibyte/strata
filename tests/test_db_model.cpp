// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Randomized model checking: long random sequences of writes, reads,
// iterations, snapshots, flushes, compactions and reopens, with every read
// compared against a std::map.

#include <functional>

#include "doctest.h"
#include "strata/comparator.hpp"
#include "support/test_util.hpp"

using namespace strata;
using namespace strata::test;

namespace {

struct KeyLess {
    const Comparator* cmp;
    bool operator()(const std::string& a, const std::string& b) const { return cmp->compare(a, b) < 0; }
};

using Model = std::map<std::string, std::string, KeyLess>;

struct Config {
    const char* name;
    Options options;
    int steps;
    bool use_disk = false;
};

class ModelChecker {
public:
    ModelChecker(const Config& config, std::uint64_t seed)
        : config_(config), rnd_(seed), model_(KeyLess{comparator()}) {
        if (config_.use_disk) {
            disk_ = std::make_unique<TempDir>();
            options_ = config_.options;
            options_.env = Env::default_env();
            options_.create_if_missing = true;
            path_ = disk_->file("db");
        } else {
            env_ = std::make_unique<MemEnv>();
            options_ = config_.options;
            options_.env = env_.get();
            options_.create_if_missing = true;
            path_ = "/model";
        }
        REQUIRE(DB::open(options_, path_, &db_).is_ok());
    }

    ~ModelChecker() {
        release_all_snapshots();
        db_.reset();
    }

    void run() {
        for (int step = 0; step < config_.steps; ++step) {
            step_ = step;
            const auto choice = rnd_.uniform(1000);
            if (choice < 350) {
                op_put();
            } else if (choice < 450) {
                op_remove();
            } else if (choice < 550) {
                op_batch();
            } else if (choice < 700) {
                op_get();
            } else if (choice < 800) {
                op_iterate();
            } else if (choice < 830) {
                op_take_snapshot();
            } else if (choice < 870) {
                op_check_snapshot();
            } else if (choice < 890) {
                REQUIRE(db_->flush().is_ok());
            } else if (choice < 905) {
                op_compact();
            } else if (choice < 910) {
                op_reopen();
            } else {
                op_put();
            }
        }
        // Final full comparison, forwards and backwards.
        check_full(ReadOptions(), model_);
        for (auto& s : snapshots_) {
            ReadOptions ro;
            ro.snapshot = s.snapshot;
            check_full(ro, s.model);
        }
    }

private:
    const Comparator* comparator() const {
        return config_.options.comparator != nullptr ? config_.options.comparator : bytewise_comparator();
    }

    std::string random_key() {
        // A small key space so keys are overwritten and deleted often, with
        // shared prefixes and a few binary keys.
        const auto n = rnd_.uniform(600);
        std::string k = "k" + std::to_string(n % 7) + "/" + std::to_string(n);
        if (n % 50 == 0) k += std::string("\x00\xff", 2);
        return k;
    }

    std::string random_value() {
        const auto kind = rnd_.uniform(10);
        if (kind == 0) return "";
        if (kind == 1) return random_string(rnd_, 1000 + rnd_.uniform(4000));
        return rnd_.printable(rnd_.uniform(120));
    }

    void op_put() {
        const std::string k = random_key();
        const std::string v = random_value();
        WriteOptions w;
        w.sync = rnd_.one_in(50);
        REQUIRE(db_->put(w, k, v).is_ok());
        model_[k] = v;
    }

    void op_remove() {
        // Mostly remove keys that exist.
        std::string k = random_key();
        if (!model_.empty() && rnd_.one_in(2)) {
            auto it = model_.lower_bound(k);
            if (it != model_.end()) k = it->first;
        }
        REQUIRE(db_->remove(WriteOptions(), k).is_ok());
        model_.erase(k);
    }

    void op_batch() {
        WriteBatch batch;
        const int n = 1 + static_cast<int>(rnd_.uniform(20));
        Model pending = model_;
        for (int i = 0; i < n; ++i) {
            const std::string k = random_key();
            if (rnd_.one_in(3)) {
                batch.remove(k);
                pending.erase(k);
            } else {
                const std::string v = random_value();
                batch.put(k, v);
                pending[k] = v;
            }
        }
        REQUIRE(db_->write(WriteOptions(), &batch).is_ok());
        model_ = std::move(pending);
    }

    void op_get() {
        for (int i = 0; i < 5; ++i) {
            const std::string k = random_key();
            std::string value;
            Status s = db_->get(ReadOptions(), k, &value);
            auto it = model_.find(k);
            INFO("step " << step_ << " key " << escape(k));
            if (it == model_.end()) {
                REQUIRE(s.is_not_found());
            } else {
                REQUIRE(s.is_ok());
                REQUIRE(value == it->second);
            }
        }
    }

    // Seeks to a random key and walks a few steps, changing direction at
    // random, comparing with the model.
    void op_iterate() { check_walk(ReadOptions(), model_); }

    void check_walk(const ReadOptions& ro, const Model& model) {
        auto it = db_->new_iterator(ro);
        auto m = model.end();
        const auto start = rnd_.uniform(4);
        if (start == 0) {
            it->seek_to_first();
            m = model.begin();
        } else if (start == 1) {
            it->seek_to_last();
            m = model.empty() ? model.end() : std::prev(model.end());
        } else {
            const std::string target = random_key();
            it->seek(target);
            m = model.lower_bound(target);
        }
        for (int i = 0; i < 30; ++i) {
            INFO("step " << step_ << " walk " << i);
            if (m == model.end()) {
                REQUIRE_FALSE(it->valid());
                break;
            }
            REQUIRE(it->valid());
            REQUIRE(std::string(it->key()) == m->first);
            REQUIRE(std::string(it->value()) == m->second);
            if (rnd_.one_in(3)) {
                it->prev();
                if (m == model.begin()) {
                    m = model.end();
                } else {
                    --m;
                }
            } else {
                it->next();
                ++m;
            }
        }
        REQUIRE(it->status().is_ok());
    }

    void check_full(const ReadOptions& ro, const Model& model) {
        auto it = db_->new_iterator(ro);
        auto m = model.begin();
        for (it->seek_to_first(); it->valid(); it->next(), ++m) {
            REQUIRE(m != model.end());
            REQUIRE(std::string(it->key()) == m->first);
            REQUIRE(std::string(it->value()) == m->second);
        }
        REQUIRE(m == model.end());
        auto r = model.rbegin();
        for (it->seek_to_last(); it->valid(); it->prev(), ++r) {
            REQUIRE(r != model.rend());
            REQUIRE(std::string(it->key()) == r->first);
        }
        REQUIRE(r == model.rend());
        REQUIRE(it->status().is_ok());
    }

    void op_take_snapshot() {
        if (snapshots_.size() >= 8) return;
        snapshots_.push_back({db_->get_snapshot(), model_});
    }

    void op_check_snapshot() {
        if (snapshots_.empty()) return;
        const std::size_t i = rnd_.uniform(snapshots_.size());
        ReadOptions ro;
        ro.snapshot = snapshots_[i].snapshot;
        const Model& model = snapshots_[i].model;
        for (int j = 0; j < 5; ++j) {
            const std::string k = random_key();
            std::string value;
            Status s = db_->get(ro, k, &value);
            auto it = model.find(k);
            INFO("step " << step_ << " snapshot get " << escape(k));
            if (it == model.end()) {
                REQUIRE(s.is_not_found());
            } else {
                REQUIRE(s.is_ok());
                REQUIRE(value == it->second);
            }
        }
        check_walk(ro, model);
        if (rnd_.one_in(2)) {
            db_->release_snapshot(snapshots_[i].snapshot);
            snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    void op_compact() {
        if (rnd_.one_in(3)) {
            REQUIRE(db_->compact_range(std::nullopt, std::nullopt).is_ok());
            return;
        }
        std::string a = random_key(), b = random_key();
        if (comparator()->compare(a, b) > 0) std::swap(a, b);
        REQUIRE(db_->compact_range(std::string_view(a), std::string_view(b)).is_ok());
    }

    void op_reopen() {
        release_all_snapshots();  // snapshots do not survive a close
        db_.reset();
        REQUIRE(DB::open(options_, path_, &db_).is_ok());
    }

    void release_all_snapshots() {
        for (auto& s : snapshots_) db_->release_snapshot(s.snapshot);
        snapshots_.clear();
    }

    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            const auto u = static_cast<unsigned char>(c);
            if (u >= 0x20 && u < 0x7f) {
                out.push_back(c);
            } else {
                out += "\\x" + std::to_string(u);
            }
        }
        return out;
    }

    struct SnapshotState {
        const Snapshot* snapshot;
        Model model;
    };

    Config config_;
    Random rnd_;
    std::unique_ptr<MemEnv> env_;
    std::unique_ptr<TempDir> disk_;
    Options options_;
    std::string path_;
    std::unique_ptr<DB> db_;
    Model model_;
    std::vector<SnapshotState> snapshots_;
    int step_ = 0;
};

Options small() {
    Options o;
    o.write_buffer_size = 32 * 1024;
    o.target_file_size = 32 * 1024;
    o.max_bytes_for_level_base = 128 * 1024;
    o.block_size = 1024;
    o.block_cache_bytes = 128 * 1024;
    return o;
}

}  // namespace

TEST_SUITE("db-model") {
    TEST_CASE("default configuration") {
        Config config{"default", small(), 6000};
        for (std::uint64_t seed : {1u, 2u, 3u}) {
            CAPTURE(seed);
            ModelChecker(config, seed).run();
        }
    }

    TEST_CASE("no compression, no filters, restart interval 1") {
        Options o = small();
        o.compression = Compression::None;
        o.bloom_bits_per_key = 0;
        o.block_restart_interval = 1;
        Config config{"plain", o, 4000};
        ModelChecker(config, 11).run();
    }

    TEST_CASE("tiny files and aggressive compaction") {
        Options o = small();
        o.write_buffer_size = 4 * 1024;
        o.target_file_size = 2 * 1024;
        o.max_bytes_for_level_base = 8 * 1024;
        o.level_size_multiplier = 4;
        o.level0_compaction_trigger = 2;
        o.level0_slowdown_trigger = 3;
        o.level0_stop_trigger = 5;
        o.block_size = 256;
        o.num_levels = 4;
        Config config{"tiny", o, 4000};
        ModelChecker(config, 21).run();
    }

    TEST_CASE("no block cache and memtables always at level 0") {
        Options o = small();
        o.block_cache_bytes = 0;
        o.max_memtable_level = 0;
        o.max_open_files = 20;  // forces table cache evictions
        Config config{"nocache", o, 4000};
        ModelChecker(config, 31).run();
    }

    TEST_CASE("reverse key order") {
        Options o = small();
        o.comparator = reverse_bytewise_comparator();
        Config config{"reverse", o, 4000};
        ModelChecker(config, 41).run();
    }

    TEST_CASE("on the local file system") {
        Config config{"disk", small(), 2500, /*use_disk=*/true};
        ModelChecker(config, 51).run();
    }
}
