// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Non-cryptographic hashing for bloom filters and cache sharding.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace strata {

/// 32-bit hash of data, mixing in `seed`. Stable across platforms and
/// releases: bloom filters stored in table files depend on it.
std::uint32_t hash32(const char* data, std::size_t n, std::uint32_t seed) noexcept;

inline std::uint32_t hash32(std::string_view data, std::uint32_t seed = 0x1f3d5b79u) noexcept {
    return hash32(data.data(), data.size(), seed);
}

}  // namespace strata
