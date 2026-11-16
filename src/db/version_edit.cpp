// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/version_edit.hpp"

#include "util/coding.hpp"
#include "util/strutil.hpp"

namespace strata {
namespace {

enum Tag : std::uint32_t {
    kComparator = 1,
    kLogNumber = 2,
    kNextFileNumber = 3,
    kLastSequence = 4,
    kCompactPointer = 5,
    kDeletedFile = 6,
    kNewFile = 7,
};

// Levels are bounded by Options::num_levels (at most 20); anything larger in
// a MANIFEST is damage.
constexpr std::uint32_t kMaxEncodedLevel = 64;

bool get_level(std::string_view& input, int* level) {
    std::uint32_t v = 0;
    if (!get_varint32(input, &v) || v >= kMaxEncodedLevel) return false;
    *level = static_cast<int>(v);
    return true;
}

bool get_internal_key(std::string_view& input, InternalKey* key) {
    std::string_view encoded;
    if (!get_length_prefixed(input, &encoded) || encoded.size() < kInternalKeyTagSize) return false;
    return key->decode_from(encoded);
}

}  // namespace

void VersionEdit::clear() {
    *this = VersionEdit();
}

void VersionEdit::add_file(int level, std::uint64_t number, std::uint64_t size, const InternalKey& smallest,
                           const InternalKey& largest) {
    FileMetaData f;
    f.number = number;
    f.file_size = size;
    f.smallest = smallest;
    f.largest = largest;
    new_files_.emplace_back(level, std::move(f));
}

void VersionEdit::encode_to(std::string& dst) const {
    if (has_comparator_) {
        put_varint32(dst, kComparator);
        put_length_prefixed(dst, comparator_);
    }
    if (has_log_number_) {
        put_varint32(dst, kLogNumber);
        put_varint64(dst, log_number_);
    }
    if (has_next_file_number_) {
        put_varint32(dst, kNextFileNumber);
        put_varint64(dst, next_file_number_);
    }
    if (has_last_sequence_) {
        put_varint32(dst, kLastSequence);
        put_varint64(dst, last_sequence_);
    }
    for (const auto& [level, key] : compact_pointers_) {
        put_varint32(dst, kCompactPointer);
        put_varint32(dst, static_cast<std::uint32_t>(level));
        put_length_prefixed(dst, key.encode());
    }
    for (const auto& [level, number] : deleted_files_) {
        put_varint32(dst, kDeletedFile);
        put_varint32(dst, static_cast<std::uint32_t>(level));
        put_varint64(dst, number);
    }
    for (const auto& [level, f] : new_files_) {
        put_varint32(dst, kNewFile);
        put_varint32(dst, static_cast<std::uint32_t>(level));
        put_varint64(dst, f.number);
        put_varint64(dst, f.file_size);
        put_length_prefixed(dst, f.smallest.encode());
        put_length_prefixed(dst, f.largest.encode());
    }
}

Status VersionEdit::decode_from(std::string_view src) {
    clear();
    std::string_view input = src;
    const char* error = nullptr;
    std::uint32_t tag = 0;
    while (error == nullptr && !input.empty()) {
        if (!get_varint32(input, &tag)) {
            error = "tag";
            break;
        }
        int level = 0;
        std::uint64_t number = 0;
        InternalKey key;
        FileMetaData f;
        std::string_view str;
        switch (tag) {
            case kComparator:
                if (get_length_prefixed(input, &str)) {
                    set_comparator_name(str);
                } else {
                    error = "comparator name";
                }
                break;
            case kLogNumber:
                if (get_varint64(input, &log_number_)) {
                    has_log_number_ = true;
                } else {
                    error = "log number";
                }
                break;
            case kNextFileNumber:
                if (get_varint64(input, &next_file_number_)) {
                    has_next_file_number_ = true;
                } else {
                    error = "next file number";
                }
                break;
            case kLastSequence:
                if (get_varint64(input, &last_sequence_)) {
                    has_last_sequence_ = true;
                } else {
                    error = "last sequence number";
                }
                break;
            case kCompactPointer:
                if (get_level(input, &level) && get_internal_key(input, &key)) {
                    compact_pointers_.emplace_back(level, std::move(key));
                } else {
                    error = "compaction pointer";
                }
                break;
            case kDeletedFile:
                if (get_level(input, &level) && get_varint64(input, &number)) {
                    deleted_files_.emplace(level, number);
                } else {
                    error = "deleted file";
                }
                break;
            case kNewFile:
                if (get_level(input, &level) && get_varint64(input, &f.number) && get_varint64(input, &f.file_size) &&
                    get_internal_key(input, &f.smallest) && get_internal_key(input, &f.largest)) {
                    new_files_.emplace_back(level, std::move(f));
                } else {
                    error = "new-file entry";
                }
                break;
            default:
                error = "unknown tag";
                break;
        }
    }
    if (error != nullptr) return Status::corruption("VersionEdit", error);
    return Status::ok();
}

std::string VersionEdit::debug_string() const {
    std::string r = "VersionEdit {";
    if (has_comparator_) r += "\n  Comparator: " + comparator_;
    if (has_log_number_) r += "\n  LogNumber: " + std::to_string(log_number_);
    if (has_next_file_number_) r += "\n  NextFile: " + std::to_string(next_file_number_);
    if (has_last_sequence_) r += "\n  LastSeq: " + std::to_string(last_sequence_);
    for (const auto& [level, key] : compact_pointers_) {
        r += "\n  CompactPointer: " + std::to_string(level) + " " + key.debug_string();
    }
    for (const auto& [level, number] : deleted_files_) {
        r += "\n  RemoveFile: " + std::to_string(level) + " " + std::to_string(number);
    }
    for (const auto& [level, f] : new_files_) {
        r += "\n  AddFile: " + std::to_string(level) + " " + std::to_string(f.number) + " " +
             std::to_string(f.file_size) + " " + f.smallest.debug_string() + " .. " + f.largest.debug_string();
    }
    r += "\n}\n";
    return r;
}

}  // namespace strata
