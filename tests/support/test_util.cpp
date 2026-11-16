// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "support/test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace strata::test {

TempDir::TempDir() {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    namespace fs = std::filesystem;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string name = "strata_test_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1));
        const fs::path p = fs::temp_directory_path() / name;
        std::error_code ec;
        if (fs::create_directory(p, ec)) {
            path_ = p.generic_string();
            return;
        }
    }
    throw std::runtime_error("cannot create a temporary directory");
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::string key(std::uint64_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08llu", static_cast<unsigned long long>(i));
    return buf;
}

std::string random_string(Random& rnd, std::size_t len) {
    std::string s(len, '\0');
    for (auto& c : s) c = static_cast<char>(rnd.uniform(256));
    return s;
}

std::string value_for(std::uint64_t i, std::size_t len) {
    std::string v = "value-" + std::to_string(i) + "-";
    while (v.size() < len) v += static_cast<char>('a' + (v.size() + i) % 26);
    v.resize(len);
    return v;
}

std::map<std::string, std::string> contents(DB& db, const ReadOptions& options) {
    std::map<std::string, std::string> result;
    auto it = db.new_iterator(options);
    for (it->seek_to_first(); it->valid(); it->next()) result.emplace(std::string(it->key()), std::string(it->value()));
    if (!it->status().is_ok()) throw std::runtime_error("iterator error: " + it->status().to_string());
    return result;
}

std::map<std::string, std::string> contents_reverse(DB& db, const ReadOptions& options) {
    std::map<std::string, std::string> result;
    auto it = db.new_iterator(options);
    for (it->seek_to_last(); it->valid(); it->prev()) result.emplace(std::string(it->key()), std::string(it->value()));
    if (!it->status().is_ok()) throw std::runtime_error("iterator error: " + it->status().to_string());
    return result;
}

std::string get(DB& db, std::string_view k, const Snapshot* snapshot) {
    ReadOptions options;
    options.snapshot = snapshot;
    std::string value;
    Status s = db.get(options, k, &value);
    if (s.is_not_found()) return "NOT_FOUND";
    if (!s.is_ok()) return s.to_string();
    return value;
}

Options small_options(Env* env) {
    Options o;
    o.env = env;
    o.create_if_missing = true;
    o.write_buffer_size = 32 * 1024;
    o.target_file_size = 32 * 1024;
    o.max_bytes_for_level_base = 128 * 1024;
    o.block_size = 1024;
    o.block_cache_bytes = 256 * 1024;
    return o;
}

MemDB::MemDB(std::unique_ptr<MemEnv> env, Options options) : env_(std::move(env)), options_(options) {
    options_.env = env_.get();
    options_.create_if_missing = true;
    // Sizes the caller left at their defaults get the small test values.
    const Options defaults;
    const Options small = small_options(env_.get());
    if (options_.write_buffer_size == defaults.write_buffer_size) options_.write_buffer_size = small.write_buffer_size;
    if (options_.target_file_size == defaults.target_file_size) options_.target_file_size = small.target_file_size;
    if (options_.max_bytes_for_level_base == defaults.max_bytes_for_level_base) {
        options_.max_bytes_for_level_base = small.max_bytes_for_level_base;
    }
    if (options_.block_size == defaults.block_size) options_.block_size = small.block_size;
    if (options_.block_cache_bytes == defaults.block_cache_bytes) options_.block_cache_bytes = small.block_cache_bytes;
    Status s = open();
    if (!s.is_ok()) throw std::runtime_error("open failed: " + s.to_string());
}

Status MemDB::open() {
    return DB::open(options_, path_, &db_);
}

Status MemDB::reopen() {
    db_.reset();
    return open();
}

}  // namespace strata::test
