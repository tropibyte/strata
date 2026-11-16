// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "db/log_format.hpp"
#include "strata/env.hpp"

namespace strata::log {

class Writer {
public:
    /// Appends records to `dest`, which already holds `dest_length` bytes
    /// (0 for a new file). Does not take ownership.
    explicit Writer(WritableFile* dest, std::uint64_t dest_length = 0);

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Appends one record and flushes it to the operating system. The caller
    /// syncs the file if it needs durability.
    Status add_record(std::string_view record);

private:
    Status emit_fragment(RecordType type, const char* data, std::size_t length);

    WritableFile* dest_;
    std::size_t block_offset_;  // position within the current block

    // CRC of each type byte, precomputed: the record CRC extends it.
    std::array<std::uint32_t, kMaxRecordType + 1> type_crc_{};
};

}  // namespace strata::log
