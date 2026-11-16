// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// CRC-32C (Castagnoli), the checksum on every log record and table block.
// Portable table-driven implementation, eight bytes per step.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace strata::crc32c {

/// CRC of data appended to a stream whose CRC so far is `crc`.
std::uint32_t extend(std::uint32_t crc, const char* data, std::size_t n) noexcept;

inline std::uint32_t value(const char* data, std::size_t n) noexcept {
    return extend(0, data, n);
}
inline std::uint32_t value(std::string_view data) noexcept {
    return extend(0, data.data(), data.size());
}

// A CRC stored next to the data it covers is masked, so that computing the
// CRC of a buffer that itself contains CRCs does not produce degenerate
// results. Masking is a rotation plus a constant.
inline constexpr std::uint32_t kMaskDelta = 0x6b2f9e45u;

inline std::uint32_t mask(std::uint32_t crc) noexcept {
    return ((crc >> 13) | (crc << 19)) + kMaskDelta;
}

inline std::uint32_t unmask(std::uint32_t masked) noexcept {
    const std::uint32_t rot = masked - kMaskDelta;
    return (rot >> 19) | (rot << 13);
}

}  // namespace strata::crc32c
