// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Internal keys. Every entry the database stores is keyed by
//     user_key | fixed64( sequence << 8 | type )
// where `sequence` is the 56-bit number of the write that made it and `type`
// says whether it is a value or a deletion marker ("tombstone").
//
// Internal keys sort by user key ascending, then sequence descending, then
// type descending — so for any user key, the newest entry comes first, and a
// seek to (user_key, S) lands on the newest entry no newer than S.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "strata/comparator.hpp"

namespace strata {

using SequenceNumber = std::uint64_t;

enum class ValueType : std::uint8_t {
    Deletion = 0,
    Value = 1,
};

/// The type to use when seeking: it is the largest, so (k, S, kValueTypeForSeek)
/// sorts before every entry for k with sequence S.
inline constexpr ValueType kValueTypeForSeek = ValueType::Value;

/// Sequence numbers fit in 56 bits; the low 8 bits of the tag hold the type.
inline constexpr SequenceNumber kMaxSequenceNumber = (std::uint64_t{1} << 56) - 1;

inline constexpr std::size_t kInternalKeyTagSize = 8;

inline std::uint64_t pack_sequence_and_type(SequenceNumber seq, ValueType type) noexcept {
    return (seq << 8) | static_cast<std::uint64_t>(type);
}

struct ParsedInternalKey {
    std::string_view user_key;
    SequenceNumber sequence = 0;
    ValueType type = ValueType::Value;

    ParsedInternalKey() = default;
    ParsedInternalKey(std::string_view u, SequenceNumber s, ValueType t) : user_key(u), sequence(s), type(t) {}

    std::string debug_string() const;
};

void append_internal_key(std::string& dst, const ParsedInternalKey& key);

/// False if `internal_key` is too short or has an unknown type.
bool parse_internal_key(std::string_view internal_key, ParsedInternalKey* out);

/// The user-key part. Requires internal_key.size() >= 8.
inline std::string_view extract_user_key(std::string_view internal_key) noexcept {
    return internal_key.substr(0, internal_key.size() - kInternalKeyTagSize);
}

/// For table filters: like extract_user_key, but tolerates short keys.
std::string_view filter_user_key(std::string_view internal_key);

/// Readable form of an internal key: 'user_key' @ seq : type.
std::string debug_internal_key(std::string_view internal_key);

/// Orders internal keys: by user comparator, then by tag descending.
class InternalKeyComparator final : public Comparator {
public:
    explicit InternalKeyComparator(const Comparator* user) : user_(user) {}

    int compare(std::string_view a, std::string_view b) const override;
    std::string_view name() const override { return "strata.InternalKeyComparator"; }
    void find_shortest_separator(std::string& start, std::string_view limit) const override;
    void find_short_successor(std::string& key) const override;

    const Comparator* user_comparator() const noexcept { return user_; }

private:
    const Comparator* user_;
};

/// An owned internal key. Used for file boundaries and compaction pointers.
class InternalKey {
public:
    InternalKey() = default;
    InternalKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
        append_internal_key(rep_, ParsedInternalKey(user_key, seq, type));
    }

    /// False if `encoded` is not a valid internal key.
    bool decode_from(std::string_view encoded) {
        rep_.assign(encoded);
        return !rep_.empty();
    }

    std::string_view encode() const noexcept { return rep_; }
    std::string_view user_key() const noexcept { return extract_user_key(rep_); }
    bool empty() const noexcept { return rep_.empty(); }
    void clear() { rep_.clear(); }

    std::string debug_string() const { return debug_internal_key(rep_); }

private:
    std::string rep_;
};

inline int compare_internal_keys(const InternalKeyComparator& cmp, const InternalKey& a, const InternalKey& b) {
    return cmp.compare(a.encode(), b.encode());
}

/// The key the memtable and tables are searched with for a point lookup:
/// varint32(length) | user_key | tag(sequence, kValueTypeForSeek).
class LookupKey {
public:
    LookupKey(std::string_view user_key, SequenceNumber sequence);

    LookupKey(const LookupKey&) = delete;
    LookupKey& operator=(const LookupKey&) = delete;

    /// Length-prefixed internal key, as the memtable stores it.
    std::string_view memtable_key() const noexcept { return rep_; }
    std::string_view internal_key() const noexcept { return std::string_view(rep_).substr(key_start_); }
    std::string_view user_key() const noexcept {
        return std::string_view(rep_).substr(key_start_, rep_.size() - key_start_ - kInternalKeyTagSize);
    }

private:
    std::string rep_;
    std::size_t key_start_ = 0;
};

}  // namespace strata
