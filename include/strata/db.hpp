// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// DB: a persistent, ordered map from byte-string keys to byte-string values.
//
//     strata::Options options;
//     options.create_if_missing = true;
//     std::unique_ptr<strata::DB> db;
//     strata::Status s = strata::DB::open(options, "/tmp/demo", &db);
//     s = db->put({}, "fruit", "mango");
//     std::string v;
//     s = db->get({}, "fruit", &v);
//
// All methods are safe to call from several threads at once.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "strata/iterator.hpp"
#include "strata/options.hpp"
#include "strata/status.hpp"
#include "strata/write_batch.hpp"

namespace strata {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

/// A consistent, read-only view of the database at one point in time.
/// Obtained from DB::get_snapshot() and passed in ReadOptions.
class Snapshot {
public:
    /// The sequence number of the last write visible in this snapshot.
    virtual std::uint64_t sequence() const noexcept = 0;

protected:
    virtual ~Snapshot() = default;
};

/// A half-open key range [start, limit).
struct Range {
    std::string_view start;
    std::string_view limit;
};

class DB {
public:
    /// Opens the database at `path`. On success *out holds the database.
    static Status open(const Options& options, const std::string& path, std::unique_ptr<DB>* out);

    DB() = default;
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    /// Waits for background work to finish, then closes.
    virtual ~DB() = default;

    virtual Status put(const WriteOptions& options, std::string_view key, std::string_view value) = 0;

    /// Removing a key that does not exist is not an error.
    virtual Status remove(const WriteOptions& options, std::string_view key) = 0;

    /// Applies every update in the batch atomically.
    virtual Status write(const WriteOptions& options, WriteBatch* batch) = 0;

    /// Stores the value for `key` in *value. Returns NotFound if the key does
    /// not exist (and leaves *value alone).
    virtual Status get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

    /// An iterator over the whole database, initially not positioned. It sees
    /// a consistent view (options.snapshot, or the state when it was made).
    /// Destroy every iterator before the database.
    virtual std::unique_ptr<Iterator> new_iterator(const ReadOptions& options) = 0;

    /// Captures the current state. Release with release_snapshot().
    virtual const Snapshot* get_snapshot() = 0;
    virtual void release_snapshot(const Snapshot* snapshot) = 0;

    /// Reports internal state. Known properties:
    ///   "strata.num-files-at-level<N>"  number of table files in level N
    ///   "strata.stats"                  per-level sizes and compaction stats
    ///   "strata.sstables"               every table file, by level
    ///   "strata.approximate-memory-usage"  bytes used by memtables and caches
    ///   "strata.last-sequence"          sequence number of the latest write
    ///   "strata.background-error"       the error that stopped writes, if any
    /// Returns false for an unknown property.
    virtual bool get_property(std::string_view property, std::string* value) = 0;

    /// For each range, the approximate bytes of table files it spans. Data
    /// still in memtables is not counted.
    virtual void get_approximate_sizes(std::span<const Range> ranges, std::span<std::uint64_t> sizes) = 0;

    /// Compacts the key range [begin, end] (nullopt means unbounded) so that
    /// deleted and overwritten data in it is discarded. compact_range(nullopt,
    /// nullopt) compacts the whole database.
    virtual Status compact_range(std::optional<std::string_view> begin, std::optional<std::string_view> end) = 0;

    /// Writes the memtable to a table file and waits for it to finish.
    virtual Status flush() = 0;
};

/// Deletes the database at `path` and everything in it. Fails if the
/// database is open.
Status destroy_db(const std::string& path, const Options& options);

}  // namespace strata
