// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// String helpers for printing keys, values and sizes.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

/// Printable ASCII is kept; other bytes become \xNN, and backslash becomes \\.
std::string escape_string(std::string_view s);

/// Reverses escape_string (also accepts \n, \t, \r, \0). False on a bad escape.
bool unescape_string(std::string_view s, std::string* out);

/// Lower-case hex, two digits per byte.
std::string to_hex(std::string_view s);

/// Parses hex (case-insensitive, even length). False if malformed.
bool from_hex(std::string_view s, std::string* out);

/// Consumes a leading run of decimal digits. False if there are none or the
/// number overflows.
bool consume_decimal(std::string_view& in, std::uint64_t* value);

/// "1.5 MiB", "312 B".
std::string human_bytes(std::uint64_t bytes);

/// Number with thousands separators: 1234567 -> "1,234,567".
std::string with_commas(std::uint64_t n);

}  // namespace strata
