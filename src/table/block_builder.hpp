// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// BlockBuilder: builds a block of sorted key/value entries with prefix
// compression.
//
// Each entry stores only the part of its key that differs from the previous
// key:
//     varint32 shared_bytes | varint32 unshared_bytes | varint32 value_length
//     key[shared_bytes..] | value
// Every `restart_interval` entries the prefix compression restarts
// (shared_bytes = 0); the offsets of these restart points are listed at the
// end of the block so a reader can binary-search them:
//     fixed32 restart[0] ... fixed32 restart[n-1] | fixed32 n

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

class Comparator;

class BlockBuilder {
public:
    /// comparator is used only to assert ordering in debug builds; may be null.
    BlockBuilder(int restart_interval, const Comparator* comparator);

    /// Keys must be added in strictly increasing order.
    void add(std::string_view key, std::string_view value);

    /// Finishes the block and returns its contents, valid until reset().
    std::string_view finish();

    void reset();

    /// Size of the block if finish() were called now.
    std::size_t current_size_estimate() const;

    bool empty() const noexcept { return buffer_.empty(); }
    std::size_t num_entries() const noexcept { return entries_; }

private:
    int restart_interval_;
    [[maybe_unused]] const Comparator* comparator_;  // used by asserts only
    std::string buffer_;
    std::vector<std::uint32_t> restarts_;
    int counter_ = 0;  // entries since the last restart
    std::size_t entries_ = 0;
    bool finished_ = false;
    std::string last_key_;
};

}  // namespace strata
