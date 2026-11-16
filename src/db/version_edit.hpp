// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// VersionEdit: a change to the set of table files (files added and removed
// per level) plus bookkeeping counters. The MANIFEST is a log of encoded
// edits; replaying them from the start reconstructs the current version.
//
// Encoding: a sequence of (varint32 tag, fields) pairs:
//     1 comparator        length-prefixed name
//     2 log number        varint64
//     3 next file number  varint64
//     4 last sequence     varint64
//     5 compact pointer   varint32 level, length-prefixed internal key
//     6 deleted file      varint32 level, varint64 number
//     7 new file          varint32 level, varint64 number, varint64 size,
//                         length-prefixed smallest and largest internal keys

#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.hpp"
#include "strata/status.hpp"

namespace strata {

struct FileMetaData {
    std::uint64_t number = 0;
    std::uint64_t file_size = 0;
    InternalKey smallest;  // smallest internal key in the table
    InternalKey largest;   // largest internal key in the table
};

using FileMetaPtr = std::shared_ptr<const FileMetaData>;

class VersionEdit {
public:
    void clear();

    void set_comparator_name(std::string_view name) {
        has_comparator_ = true;
        comparator_.assign(name);
    }
    void set_log_number(std::uint64_t n) {
        has_log_number_ = true;
        log_number_ = n;
    }
    void set_next_file(std::uint64_t n) {
        has_next_file_number_ = true;
        next_file_number_ = n;
    }
    void set_last_sequence(SequenceNumber s) {
        has_last_sequence_ = true;
        last_sequence_ = s;
    }
    void set_compact_pointer(int level, const InternalKey& key) { compact_pointers_.emplace_back(level, key); }

    void add_file(int level, const FileMetaData& file) { new_files_.emplace_back(level, file); }
    void add_file(int level, std::uint64_t number, std::uint64_t size, const InternalKey& smallest,
                  const InternalKey& largest);
    void remove_file(int level, std::uint64_t number) { deleted_files_.emplace(level, number); }

    void encode_to(std::string& dst) const;
    Status decode_from(std::string_view src);

    std::string debug_string() const;

    bool has_comparator() const noexcept { return has_comparator_; }
    const std::string& comparator() const noexcept { return comparator_; }
    bool has_log_number() const noexcept { return has_log_number_; }
    std::uint64_t log_number() const noexcept { return log_number_; }
    bool has_next_file_number() const noexcept { return has_next_file_number_; }
    std::uint64_t next_file_number() const noexcept { return next_file_number_; }
    bool has_last_sequence() const noexcept { return has_last_sequence_; }
    SequenceNumber last_sequence() const noexcept { return last_sequence_; }

    using DeletedFileSet = std::set<std::pair<int, std::uint64_t>>;
    const DeletedFileSet& deleted_files() const noexcept { return deleted_files_; }
    const std::vector<std::pair<int, FileMetaData>>& new_files() const noexcept { return new_files_; }
    const std::vector<std::pair<int, InternalKey>>& compact_pointers() const noexcept { return compact_pointers_; }

private:
    std::string comparator_;
    std::uint64_t log_number_ = 0;
    std::uint64_t next_file_number_ = 0;
    SequenceNumber last_sequence_ = 0;
    bool has_comparator_ = false;
    bool has_log_number_ = false;
    bool has_next_file_number_ = false;
    bool has_last_sequence_ = false;

    std::vector<std::pair<int, InternalKey>> compact_pointers_;
    DeletedFileSet deleted_files_;
    std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace strata
