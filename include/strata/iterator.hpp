// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Iterator: an ordered cursor over key/value pairs. Used for the public DB
// iterator and, internally, for memtables, blocks, tables and merges.
//
// An iterator is not thread-safe; give each thread its own. key() and value()
// stay valid only until the iterator is moved.

#pragma once

#include <memory>
#include <string_view>

#include "strata/status.hpp"

namespace strata {

class Iterator {
public:
    Iterator() = default;
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    virtual ~Iterator() = default;

    /// True when positioned at an entry.
    virtual bool valid() const = 0;

    virtual void seek_to_first() = 0;
    virtual void seek_to_last() = 0;

    /// Positions at the first entry with key >= target.
    virtual void seek(std::string_view target) = 0;

    /// Requires valid().
    virtual void next() = 0;
    /// Requires valid().
    virtual void prev() = 0;

    /// Requires valid().
    virtual std::string_view key() const = 0;
    /// Requires valid().
    virtual std::string_view value() const = 0;

    /// The first error encountered, if any. An iterator that hit an error is
    /// no longer valid().
    virtual Status status() const = 0;
};

/// An iterator with no entries. Its status() is `status`.
std::unique_ptr<Iterator> new_empty_iterator(Status status = Status::ok());

}  // namespace strata
