// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/dbformat.hpp"

#include <cassert>

#include "util/coding.hpp"
#include "util/strutil.hpp"

namespace strata {

void append_internal_key(std::string& dst, const ParsedInternalKey& key) {
    dst.append(key.user_key);
    put_fixed64(dst, pack_sequence_and_type(key.sequence, key.type));
}

bool parse_internal_key(std::string_view internal_key, ParsedInternalKey* out) {
    if (internal_key.size() < kInternalKeyTagSize) return false;
    const std::uint64_t tag = decode_fixed64(internal_key.data() + internal_key.size() - kInternalKeyTagSize);
    const auto type = static_cast<std::uint8_t>(tag & 0xffu);
    if (type > static_cast<std::uint8_t>(ValueType::Value)) return false;
    out->user_key = extract_user_key(internal_key);
    out->sequence = tag >> 8;
    out->type = static_cast<ValueType>(type);
    return true;
}

std::string_view filter_user_key(std::string_view internal_key) {
    if (internal_key.size() < kInternalKeyTagSize) return internal_key;
    return extract_user_key(internal_key);
}

std::string ParsedInternalKey::debug_string() const {
    std::string out = "'" + escape_string(user_key) + "' @ " + std::to_string(sequence) + " : ";
    out += type == ValueType::Value ? "put" : "del";
    return out;
}

std::string debug_internal_key(std::string_view internal_key) {
    ParsedInternalKey parsed;
    if (parse_internal_key(internal_key, &parsed)) return parsed.debug_string();
    return "(bad)" + escape_string(internal_key);
}

int InternalKeyComparator::compare(std::string_view a, std::string_view b) const {
    int r = user_->compare(extract_user_key(a), extract_user_key(b));
    if (r == 0) {
        const std::uint64_t a_tag = decode_fixed64(a.data() + a.size() - kInternalKeyTagSize);
        const std::uint64_t b_tag = decode_fixed64(b.data() + b.size() - kInternalKeyTagSize);
        if (a_tag > b_tag) {
            r = -1;
        } else if (a_tag < b_tag) {
            r = 1;
        }
    }
    return r;
}

void InternalKeyComparator::find_shortest_separator(std::string& start, std::string_view limit) const {
    // Shorten the user-key part, then give it the largest possible tag so it
    // still sorts after every entry of the original start key.
    const std::string_view user_start = extract_user_key(start);
    const std::string_view user_limit = extract_user_key(limit);
    std::string tmp(user_start);
    user_->find_shortest_separator(tmp, user_limit);
    if (tmp.size() < user_start.size() && user_->compare(user_start, tmp) < 0) {
        put_fixed64(tmp, pack_sequence_and_type(kMaxSequenceNumber, kValueTypeForSeek));
        assert(compare(start, tmp) < 0);
        assert(compare(tmp, limit) < 0);
        start.swap(tmp);
    }
}

void InternalKeyComparator::find_short_successor(std::string& key) const {
    const std::string_view user_key = extract_user_key(key);
    std::string tmp(user_key);
    user_->find_short_successor(tmp);
    if (tmp.size() < user_key.size() && user_->compare(user_key, tmp) < 0) {
        put_fixed64(tmp, pack_sequence_and_type(kMaxSequenceNumber, kValueTypeForSeek));
        assert(compare(key, tmp) < 0);
        key.swap(tmp);
    }
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
    const std::size_t internal_size = user_key.size() + kInternalKeyTagSize;
    rep_.reserve(internal_size + kMaxVarint32Bytes);
    put_varint32(rep_, static_cast<std::uint32_t>(internal_size));
    key_start_ = rep_.size();
    append_internal_key(rep_, ParsedInternalKey(user_key, sequence, kValueTypeForSeek));
}

}  // namespace strata
