// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// MemTable: the in-memory, sorted buffer of recent writes. A skiplist of
// entries, each one
//     varint32 internal_key_length | internal_key | varint32 value_length | value
// allocated in an Arena.
//
// One writer at a time (the DB's write path) and any number of concurrent
// readers. Held by shared_ptr so iterators can outlive the DB's reference.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.hpp"
#include "db/skiplist.hpp"
#include "strata/iterator.hpp"
#include "util/arena.hpp"

namespace strata {

class MemTable : public std::enable_shared_from_this<MemTable> {
public:
    explicit MemTable(const InternalKeyComparator& comparator);

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    /// Bytes allocated for entries. Safe to call while the writer adds.
    std::size_t approximate_memory_usage() const noexcept { return arena_.memory_usage(); }

    /// Iterator over internal keys. Keeps the memtable alive.
    std::unique_ptr<Iterator> new_iterator() const;

    /// Adds an entry. Requires external synchronization with other adds.
    void add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

    /// If the memtable holds an entry for key.user_key() with sequence <=
    /// the lookup sequence, returns true and: for a value, stores it in
    /// *value and sets *status OK; for a deletion, sets *status NotFound.
    /// Returns false if there is no such entry.
    bool get(const LookupKey& key, std::string* value, Status* status) const;

    std::uint64_t num_entries() const noexcept { return entries_.load(std::memory_order_relaxed); }
    std::uint64_t num_deletions() const noexcept { return deletions_.load(std::memory_order_relaxed); }

private:
    struct KeyComparator {
        const InternalKeyComparator* comparator;
        int operator()(const char* a, const char* b) const;
    };
    using Table = SkipList<const char*, KeyComparator>;

    friend class MemTableIterator;

    InternalKeyComparator comparator_;
    Arena arena_;
    Table table_;
    std::atomic<std::uint64_t> entries_{0};
    std::atomic<std::uint64_t> deletions_{0};
};

}  // namespace strata
