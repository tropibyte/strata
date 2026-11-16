// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// strata-lz: a small LZ77 block compressor for table data blocks.
//
// Compressed format:
//   varint32 uncompressed_length
//   sequence*
// sequence:
//   token          one byte; high nibble = literal count, low nibble = match
//                  length - 4. A nibble of 15 means more length bytes follow.
//   [lit_ext...]   bytes added to 15, each 255 except the last
//   literals
//   -- the stream may end here, once uncompressed_length bytes are produced --
//   offset         2 bytes little-endian, 1..65535 back from the output end
//   [match_ext...] bytes added to 15 + 4, as for literals
//
// The decoder checks every bound, so damaged input yields false rather than
// a crash or an over-read.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace strata::lz {

/// Appends the compressed form of `input` to `out`.
void compress(std::string_view input, std::string& out);

/// Reads the uncompressed length from the header.
bool uncompressed_length(std::string_view compressed, std::size_t* length);

/// Replaces `out` with the decompressed data. False if `compressed` is
/// malformed.
bool decompress(std::string_view compressed, std::string& out);

}  // namespace strata::lz
