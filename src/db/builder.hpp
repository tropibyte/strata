// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Writing table files from iterators over internal keys: memtable flushes
// use build_table(); compactions use TableFileWriter directly so they can
// roll over to a new file mid-stream.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.hpp"
#include "db/version_edit.hpp"
#include "strata/env.hpp"
#include "strata/iterator.hpp"
#include "table/table_builder.hpp"

namespace strata {

class TableCache;

/// Builds one table file and tracks what the database records about it:
/// the key range, and properties (deletions, sequence range) for dumps.
class TableFileWriter {
public:
    TableFileWriter(Env* env, const TableOptions& options, std::string path);
    ~TableFileWriter();

    Status open();

    /// Keys must be internal keys in increasing order.
    void add(std::string_view internal_key, std::string_view value);

    std::uint64_t num_entries() const;
    std::uint64_t file_size() const;
    Status status() const;

    /// Finishes, syncs and closes the file; fills meta's size and key range.
    Status finish(FileMetaData* meta);

    /// Abandons the file (the caller deletes it).
    void abandon();

    const std::string& path() const noexcept { return path_; }
    std::string_view last_user_key() const { return extract_user_key(last_key_); }
    bool empty() const { return num_entries() == 0; }

private:
    Env* env_;
    TableOptions options_;
    std::string path_;
    std::unique_ptr<WritableFile> file_;
    std::unique_ptr<TableBuilder> builder_;
    std::string first_key_;
    std::string last_key_;
    std::uint64_t deletions_ = 0;
    SequenceNumber smallest_seq_ = kMaxSequenceNumber;
    SequenceNumber largest_seq_ = 0;
    bool done_ = false;
};

/// Writes every entry of `iter` to table file meta->number. If the iterator
/// is empty, writes no file and leaves meta->file_size at 0. On success the
/// table is checked by opening it through `table_cache`.
Status build_table(const std::string& dbname, Env* env, const TableOptions& options, TableCache* table_cache,
                   Iterator* iter, FileMetaData* meta);

}  // namespace strata
