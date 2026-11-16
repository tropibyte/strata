// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Encoding of integers and strings in strata's file formats.
//
// Fixed-width integers are little-endian. Varints use 7 bits per byte, low
// bits first, with the high bit set on every byte but the last. A
// "length-prefixed" string is a varint32 length followed by the bytes.
//
// The get_* functions consume from the front of `input` and return false
// (leaving `input` unspecified) when it is too short or malformed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

inline constexpr int kMaxVarint32Bytes = 5;
inline constexpr int kMaxVarint64Bytes = 10;

void put_fixed32(std::string& dst, std::uint32_t value);
void put_fixed64(std::string& dst, std::uint64_t value);
void put_varint32(std::string& dst, std::uint32_t value);
void put_varint64(std::string& dst, std::uint64_t value);
void put_length_prefixed(std::string& dst, std::string_view value);

/// Writes into a buffer of at least 4 / 8 bytes.
void encode_fixed32(char* dst, std::uint32_t value) noexcept;
void encode_fixed64(char* dst, std::uint64_t value) noexcept;

/// Writes into a buffer of at least kMaxVarint*Bytes; returns the end.
char* encode_varint32(char* dst, std::uint32_t value) noexcept;
char* encode_varint64(char* dst, std::uint64_t value) noexcept;

std::uint32_t decode_fixed32(const char* p) noexcept;
std::uint64_t decode_fixed64(const char* p) noexcept;

/// Number of bytes the varint encoding of `value` takes.
int varint_length(std::uint64_t value) noexcept;

bool get_fixed32(std::string_view& input, std::uint32_t* value);
bool get_fixed64(std::string_view& input, std::uint64_t* value);
bool get_varint32(std::string_view& input, std::uint32_t* value);
bool get_varint64(std::string_view& input, std::uint64_t* value);
bool get_length_prefixed(std::string_view& input, std::string_view* value);

/// Pointer-based varint decoding over [p, limit). Returns the byte after the
/// varint, or nullptr if it is truncated or too long.
const char* decode_varint32(const char* p, const char* limit, std::uint32_t* value) noexcept;
const char* decode_varint64(const char* p, const char* limit, std::uint64_t* value) noexcept;

}  // namespace strata
