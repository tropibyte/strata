// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/crc32c.hpp"

#include <array>

namespace strata::crc32c {
namespace {

// Reflected Castagnoli polynomial.
constexpr std::uint32_t kPoly = 0x82f63b78u;

// tables[k][b]: CRC contribution of byte b followed by k zero bytes, so eight
// bytes can be folded in with eight lookups ("slicing by 8").
using Tables = std::array<std::array<std::uint32_t, 256>, 8>;

constexpr Tables make_tables() {
    Tables t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int j = 0; j < 8; ++j) crc = (crc & 1u) ? (crc >> 1) ^ kPoly : crc >> 1;
        t[0][i] = crc;
    }
    for (std::size_t k = 1; k < 8; ++k) {
        for (std::size_t i = 0; i < 256; ++i) {
            const std::uint32_t prev = t[k - 1][i];
            t[k][i] = (prev >> 8) ^ t[0][prev & 0xffu];
        }
    }
    return t;
}

constexpr Tables kTables = make_tables();

inline std::uint32_t load_le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::uint32_t extend(std::uint32_t crc, const char* data, std::size_t n) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    std::uint32_t c = ~crc;

    while (n >= 8) {
        const std::uint32_t lo = load_le32(p) ^ c;
        const std::uint32_t hi = load_le32(p + 4);
        c = kTables[7][lo & 0xffu] ^ kTables[6][(lo >> 8) & 0xffu] ^ kTables[5][(lo >> 16) & 0xffu] ^
            kTables[4][lo >> 24] ^ kTables[3][hi & 0xffu] ^ kTables[2][(hi >> 8) & 0xffu] ^
            kTables[1][(hi >> 16) & 0xffu] ^ kTables[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n > 0) {
        c = (c >> 8) ^ kTables[0][(c ^ *p) & 0xffu];
        ++p;
        --n;
    }
    return ~c;
}

}  // namespace strata::crc32c
