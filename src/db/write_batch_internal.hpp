// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The parts of WriteBatch only the database uses: the sequence number in the
// header, raw contents (a WAL record is exactly a batch's contents), and
// applying a batch to a memtable.

#pragma once

#include <cstdint>
#include <string_view>

#include "db/dbformat.hpp"
#include "strata/write_batch.hpp"

namespace strata {

class MemTable;

class WriteBatchInternal {
public:
    static constexpr std::size_t kHeaderSize = 12;  // fixed64 sequence + fixed32 count

    static std::uint32_t count(const WriteBatch& batch);
    static void set_count(WriteBatch& batch, std::uint32_t n);

    static SequenceNumber sequence(const WriteBatch& batch);
    static void set_sequence(WriteBatch& batch, SequenceNumber seq);

    static std::string_view contents(const WriteBatch& batch) { return batch.rep_; }
    static std::size_t byte_size(const WriteBatch& batch) { return batch.rep_.size(); }

    /// Replaces the batch with an encoded batch read from the log. Requires
    /// contents.size() >= kHeaderSize.
    static void set_contents(WriteBatch& batch, std::string_view contents);

    /// Inserts every update into the memtable, numbering them from the
    /// batch's sequence number.
    static Status insert_into(const WriteBatch& batch, MemTable* mem);

    /// Appends src's updates to dst (keeping dst's sequence number).
    static void append(WriteBatch& dst, const WriteBatch& src);
};

}  // namespace strata
