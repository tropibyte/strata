// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Comparator: the total order on user keys. A database must always be opened
// with a comparator of the same name it was created with; the name is stored
// in the MANIFEST and checked on open.

#pragma once

#include <string>
#include <string_view>

namespace strata {

class Comparator {
public:
    virtual ~Comparator() = default;

    /// Three-way comparison: < 0 if a < b, 0 if equal, > 0 if a > b.
    virtual int compare(std::string_view a, std::string_view b) const = 0;

    /// Identifies the ordering. Persisted; changing it makes old databases
    /// unreadable with the new comparator.
    virtual std::string_view name() const = 0;

    /// If start < limit, may change start to a shorter string in [start, limit).
    /// Used to shrink index-block keys. Doing nothing is always correct.
    virtual void find_shortest_separator(std::string& start, std::string_view limit) const = 0;

    /// May change key to a short string >= key. Doing nothing is always correct.
    virtual void find_short_successor(std::string& key) const = 0;
};

/// Lexicographic order on unsigned bytes (memcmp order). The default.
const Comparator* bytewise_comparator() noexcept;

/// The reverse of bytewise_comparator(). Mostly useful for tests.
const Comparator* reverse_bytewise_comparator() noexcept;

}  // namespace strata
