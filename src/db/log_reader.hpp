// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "db/log_format.hpp"
#include "strata/env.hpp"

namespace strata::log {

class Reader {
public:
    /// Told about data the reader had to skip.
    class Reporter {
    public:
        virtual ~Reporter() = default;
        /// `bytes` is an estimate of how much was dropped.
        virtual void corruption(std::size_t bytes, const Status& status) = 0;
    };

    /// Reads from `file` (not owned). `reporter` may be null. With `checksum`
    /// set, fragments whose CRC does not match are reported and skipped.
    Reader(SequentialFile* file, Reporter* reporter, bool checksum);

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Reads the next record into *record. Returns false at end of input.
    bool read_record(std::string* record);

    /// File offset of the start of the last record returned.
    std::uint64_t last_record_offset() const noexcept { return last_record_offset_; }

private:
    // Results of read_physical_record beyond the record types.
    enum : unsigned { kEof = kMaxRecordType + 1, kBadRecord = kMaxRecordType + 2 };

    unsigned read_physical_record(std::string_view* fragment);
    void report_drop(std::size_t bytes, std::string_view reason);

    SequentialFile* file_;
    Reporter* reporter_;
    bool checksum_;
    std::string backing_;
    std::string_view buffer_;  // unread part of backing_
    bool eof_ = false;         // the last read returned less than a block

    std::uint64_t buffer_end_offset_ = 0;  // file offset just past backing_
    std::uint64_t last_record_offset_ = 0;
};

}  // namespace strata::log
