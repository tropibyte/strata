// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Helpers shared by the tests.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "db/db_impl.hpp"
#include "env/mem_env.hpp"
#include "strata/db.hpp"
#include "strata/env.hpp"
#include "util/random.hpp"

namespace strata::test {

/// A fresh, empty directory under the system temp directory, removed (with
/// its contents) when the object is destroyed.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const noexcept { return path_; }
    std::string file(std::string_view name) const { return path_ + "/" + std::string(name); }

private:
    std::string path_;
};

/// Fixed-width numeric key, so numeric and lexicographic order agree.
std::string key(std::uint64_t i);

/// A random string of length len.
std::string random_string(Random& rnd, std::size_t len);

/// A compressible value of length len derived from i.
std::string value_for(std::uint64_t i, std::size_t len = 100);

/// Every key/value visible through a fresh iterator, as a map.
std::map<std::string, std::string> contents(DB& db, const ReadOptions& options = ReadOptions());

/// Same, walking backwards (to exercise prev()).
std::map<std::string, std::string> contents_reverse(DB& db, const ReadOptions& options = ReadOptions());

/// db->get as a string: the value, "NOT_FOUND", or the error.
std::string get(DB& db, std::string_view k, const Snapshot* snapshot = nullptr);

/// Small sizes so tests exercise flushes and compactions quickly.
Options small_options(Env* env);

inline DBImpl* impl(DB& db) {
    return static_cast<DBImpl*>(&db);
}

/// Opens a database in memory with test options; owns the Env.
class MemDB {
public:
    MemDB() : MemDB(std::make_unique<MemEnv>()) {}
    explicit MemDB(std::unique_ptr<MemEnv> env, Options options = {});

    DB& db() { return *db_; }
    DB* operator->() { return db_.get(); }
    MemEnv& env() { return *env_; }
    Options& options() { return options_; }
    const std::string& path() const { return path_; }

    /// Closes and reopens with the same options.
    Status reopen();
    void close() { db_.reset(); }
    Status open();

private:
    std::unique_ptr<MemEnv> env_;
    Options options_;
    std::string path_ = "/db";
    std::unique_ptr<DB> db_;
};

}  // namespace strata::test
