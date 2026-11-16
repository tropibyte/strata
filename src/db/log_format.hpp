// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Record log format, used by the write-ahead log (NNNNNN.log) and by the
// MANIFEST.
//
// The file is a sequence of 32 KiB blocks. A record is written as one or
// more fragments, each with a 7-byte header:
//     fixed32 masked CRC-32C of (type byte + payload)
//     fixed16 payload length (little-endian)
//     uint8   type: Full, or First / Middle... / Last for a record split
//             across blocks
// A fragment never straddles a block boundary. If fewer than 7 bytes remain
// in a block they are zero-filled and the next fragment starts in the next
// block.
//
// A torn write at the end of the log (the process died mid-record) reads as
// end-of-file, not as corruption.

#pragma once

#include <cstddef>
#include <cstdint>

namespace strata::log {

enum RecordType : std::uint8_t {
    kZeroType = 0,  // preallocated or zero-filled space
    kFullType = 1,
    kFirstType = 2,
    kMiddleType = 3,
    kLastType = 4,
};

inline constexpr int kMaxRecordType = kLastType;
inline constexpr std::size_t kBlockSize = 32768;
inline constexpr std::size_t kHeaderSize = 4 + 2 + 1;

}  // namespace strata::log
