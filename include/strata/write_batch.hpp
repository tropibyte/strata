// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// WriteBatch: a group of updates applied atomically by DB::write(). Either
// every update in the batch becomes visible, or none does, including across
// a crash.
//
// Updates apply in the order they were added, so
//     batch.put("k", "v1"); batch.remove("k"); batch.put("k", "v2");
// leaves "k" = "v2".

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "strata/status.hpp"

namespace strata {

class WriteBatch {
public:
    /// Receives each update in order from iterate().
    class Handler {
    public:
        virtual ~Handler() = default;
        virtual void put(std::string_view key, std::string_view value) = 0;
        virtual void remove(std::string_view key) = 0;
    };

    WriteBatch();

    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);

    /// Drops every update.
    void clear();

    /// Appends every update in `other` to this batch.
    void append(const WriteBatch& other);

    /// Number of updates.
    std::uint32_t count() const;

    /// Size in bytes of the encoded batch. Grows with each update.
    std::size_t approximate_size() const { return rep_.size(); }

    /// Calls the handler for each update, in order. Returns Corruption if the
    /// batch's encoding is damaged.
    Status iterate(Handler& handler) const;

private:
    friend class WriteBatchInternal;
    // Format: fixed64 sequence | fixed32 count | records...
    // record: kTypeValue varstring varstring | kTypeDeletion varstring
    std::string rep_;
};

}  // namespace strata
