// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/lz.hpp"

#include <array>
#include <cstdint>
#include <cstring>

#include "util/coding.hpp"

namespace strata::lz {
namespace {

constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kMaxOffset = 65535;
constexpr int kHashBits = 13;
constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
// Upper bound on the size we will allocate for damaged input that claims a
// huge length. Data blocks are a few KiB; 256 MiB is far beyond any of them.
constexpr std::size_t kMaxUncompressed = std::size_t{256} << 20;

inline std::uint32_t load32(const char* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::size_t hash4(std::uint32_t v) noexcept {
    return static_cast<std::size_t>((v * 2654435761u) >> (32 - kHashBits));
}

void put_length_ext(std::string& out, std::size_t len) {
    // len has already had 15 subtracted.
    while (len >= 255) {
        out.push_back('\xff');
        len -= 255;
    }
    out.push_back(static_cast<char>(len));
}

void emit_sequence(std::string& out, const char* literals, std::size_t lit_len, std::size_t offset,
                   std::size_t match_len) {
    const std::size_t lit_nib = lit_len < 15 ? lit_len : 15;
    std::size_t match_nib = 0;
    if (match_len != 0) {
        const std::size_t m = match_len - kMinMatch;
        match_nib = m < 15 ? m : 15;
    }
    out.push_back(static_cast<char>((lit_nib << 4) | match_nib));
    if (lit_len >= 15) put_length_ext(out, lit_len - 15);
    out.append(literals, lit_len);
    if (match_len == 0) return;
    out.push_back(static_cast<char>(offset & 0xffu));
    out.push_back(static_cast<char>((offset >> 8) & 0xffu));
    if (match_len - kMinMatch >= 15) put_length_ext(out, match_len - kMinMatch - 15);
}

// Reads an extended length: adds bytes while they are 255.
bool read_length_ext(const char*& p, const char* end, std::size_t& len) {
    for (;;) {
        if (p >= end) return false;
        const auto b = static_cast<std::uint8_t>(*p++);
        len += b;
        if (len > kMaxUncompressed) return false;
        if (b != 255) return true;
    }
}

}  // namespace

void compress(std::string_view input, std::string& out) {
    put_varint32(out, static_cast<std::uint32_t>(input.size()));
    const char* base = input.data();
    const std::size_t n = input.size();
    if (n == 0) return;

    // Positions + 1 of the last occurrence of each hashed 4-byte sequence;
    // 0 means empty.
    std::array<std::uint32_t, kHashSize> table{};

    std::size_t anchor = 0;  // start of pending literals
    std::size_t pos = 0;
    while (n >= kMinMatch && pos + kMinMatch <= n) {
        const std::uint32_t seq = load32(base + pos);
        const std::size_t h = hash4(seq);
        const std::size_t candidate_plus1 = table[h];
        table[h] = static_cast<std::uint32_t>(pos + 1);

        if (candidate_plus1 != 0) {
            const std::size_t cand = candidate_plus1 - 1;
            if (pos - cand <= kMaxOffset && load32(base + cand) == seq) {
                std::size_t len = kMinMatch;
                while (pos + len < n && base[cand + len] == base[pos + len]) ++len;
                emit_sequence(out, base + anchor, pos - anchor, pos - cand, len);
                // Seed the table with a position inside the match so the next
                // search has a recent candidate.
                if (pos + len >= 2 && pos + len + 2 <= n && len > 2) {
                    const std::size_t p2 = pos + len - 2;
                    table[hash4(load32(base + p2))] = static_cast<std::uint32_t>(p2 + 1);
                }
                pos += len;
                anchor = pos;
                continue;
            }
        }
        ++pos;
    }
    if (anchor < n) emit_sequence(out, base + anchor, n - anchor, 0, 0);
}

bool uncompressed_length(std::string_view compressed, std::size_t* length) {
    std::uint32_t len = 0;
    if (!get_varint32(compressed, &len)) return false;
    *length = len;
    return true;
}

bool decompress(std::string_view compressed, std::string& out) {
    std::uint32_t expected32 = 0;
    if (!get_varint32(compressed, &expected32)) return false;
    const std::size_t expected = expected32;
    // Each input byte can produce at most 255 + a few output bytes.
    if (expected > kMaxUncompressed || expected > (compressed.size() + 1) * 260) return false;

    out.clear();
    out.resize(expected);
    char* dst = out.data();
    std::size_t produced = 0;

    const char* p = compressed.data();
    const char* end = p + compressed.size();
    while (produced < expected) {
        if (p >= end) return false;
        const auto token = static_cast<std::uint8_t>(*p++);

        std::size_t lit_len = token >> 4;
        if (lit_len == 15 && !read_length_ext(p, end, lit_len)) return false;
        if (lit_len > static_cast<std::size_t>(end - p) || lit_len > expected - produced) return false;
        std::memcpy(dst + produced, p, lit_len);
        p += lit_len;
        produced += lit_len;
        if (produced == expected) break;  // final sequence has no match

        if (end - p < 2) return false;
        const std::size_t offset = static_cast<std::size_t>(static_cast<std::uint8_t>(p[0])) |
                                   (static_cast<std::size_t>(static_cast<std::uint8_t>(p[1])) << 8);
        p += 2;
        std::size_t match_len = token & 0x0fu;
        if (match_len == 15 && !read_length_ext(p, end, match_len)) return false;
        match_len += kMinMatch;
        if (offset == 0 || offset > produced || match_len > expected - produced) return false;

        // Byte-at-a-time copy: the source may overlap the destination (a
        // run), which memcpy does not allow.
        const char* src = dst + produced - offset;
        char* d = dst + produced;
        for (std::size_t i = 0; i < match_len; ++i) d[i] = src[i];
        produced += match_len;
    }
    return p == end;
}

}  // namespace strata::lz
