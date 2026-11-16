// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Table file (.sst) layout:
//
//   [data block 0]        each block is followed by a 5-byte trailer:
//   [data block 1]          1 byte compression type, then fixed32 masked
//   ...                     CRC-32C of the block contents and type byte
//   [filter block]        bloom filter over every user key (optional)
//   [properties block]    name -> value strings (counts, sizes, ...)
//   [metaindex block]     "filter.bloom" -> handle, "properties" -> handle
//   [index block]         one entry per data block: a key >= every key in
//                           that block and < every key in the next -> handle
//   [footer]              48 bytes: metaindex handle, index handle (varints,
//                           zero-padded to 40 bytes), fixed64 magic
//
// A block handle is varint64 offset, varint64 size (size excludes trailer).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "strata/env.hpp"
#include "strata/options.hpp"
#include "strata/status.hpp"

namespace strata {

/// "STRATKV1" read as a little-endian fixed64.
inline constexpr std::uint64_t kTableMagic = 0x3156544b41525453ull;
inline constexpr std::size_t kBlockTrailerSize = 5;

inline constexpr std::string_view kFilterBlockName = "filter.bloom";
inline constexpr std::string_view kPropertiesBlockName = "properties";

class BlockHandle {
public:
    static constexpr std::size_t kMaxEncodedLength = 20;

    BlockHandle() = default;
    BlockHandle(std::uint64_t offset, std::uint64_t size) : offset_(offset), size_(size) {}

    std::uint64_t offset() const noexcept { return offset_; }
    std::uint64_t size() const noexcept { return size_; }
    void set_offset(std::uint64_t v) noexcept { offset_ = v; }
    void set_size(std::uint64_t v) noexcept { size_ = v; }

    void encode_to(std::string& dst) const;
    /// Consumes the handle from the front of input.
    Status decode_from(std::string_view& input);

private:
    std::uint64_t offset_ = 0;
    std::uint64_t size_ = 0;
};

struct Footer {
    static constexpr std::size_t kHandlesLength = 2 * BlockHandle::kMaxEncodedLength;
    static constexpr std::size_t kEncodedLength = kHandlesLength + 8;

    BlockHandle metaindex;
    BlockHandle index;

    void encode_to(std::string& dst) const;
    /// input must be exactly the last kEncodedLength bytes of the file.
    Status decode_from(std::string_view input);
};

/// Reads the block at `handle`, checks its checksum if `verify` is set (or
/// always, for compressed blocks whose decoding would otherwise run on
/// garbage), and decompresses it into *contents.
Status read_block(const RandomAccessFile& file, const BlockHandle& handle, bool verify, std::string* contents);

/// Writes the stored form of block `raw` to *out: compressed with `wanted`
/// if that saves at least 1/8 of its size, else as is. Returns the
/// compression actually used (the trailer's type byte).
Compression compress_block(std::string_view raw, Compression wanted, std::string* out);

}  // namespace strata
