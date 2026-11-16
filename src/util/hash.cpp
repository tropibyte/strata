// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/hash.hpp"

namespace strata {
namespace {

constexpr std::uint32_t kPrime1 = 0x9e3779b1u;
constexpr std::uint32_t kPrime2 = 0x85ebca77u;
constexpr std::uint32_t kPrime3 = 0xc2b2ae3du;

inline std::uint32_t rotl(std::uint32_t x, int r) noexcept {
    return (x << r) | (x >> (32 - r));
}

inline std::uint32_t load_le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::uint32_t hash32(const char* data, std::size_t n, std::uint32_t seed) noexcept {
    // A small multiply-rotate hash in the style of xxHash32: four bytes per
    // round, then the tail, then a final avalanche.
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    std::uint32_t h = seed + kPrime3 + static_cast<std::uint32_t>(n);

    while (n >= 4) {
        h += load_le32(p) * kPrime2;
        h = rotl(h, 17) * kPrime1;
        p += 4;
        n -= 4;
    }
    while (n > 0) {
        h += static_cast<std::uint32_t>(*p) * kPrime3;
        h = rotl(h, 11) * kPrime1;
        ++p;
        --n;
    }

    h ^= h >> 15;
    h *= kPrime2;
    h ^= h >> 13;
    h *= kPrime3;
    h ^= h >> 16;
    return h;
}

}  // namespace strata
