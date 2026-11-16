// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/bloom.hpp"

#include <algorithm>
#include <cmath>

#include "util/hash.hpp"

namespace strata {
namespace {

constexpr std::uint32_t kBloomSeed = 0xbc9f1d34u;
constexpr int kMaxProbes = 30;

inline std::uint32_t probe_delta(std::uint32_t h) noexcept {
    return (h >> 15) | (h << 17);
}

}  // namespace

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key) : bits_per_key_(std::max(1, bits_per_key)) {
    // k = ln(2) * bits_per_key minimises the false-positive rate.
    k_ = static_cast<int>(std::lround(0.69 * bits_per_key_));
    k_ = std::clamp(k_, 1, kMaxProbes);
}

void BloomFilterBuilder::add_key(std::string_view key) {
    hashes_.push_back(hash32(key, kBloomSeed));
}

std::string BloomFilterBuilder::finish() {
    // Small filters have a high false-positive rate; use at least 64 bits.
    std::size_t bits = hashes_.size() * static_cast<std::size_t>(bits_per_key_);
    bits = std::max<std::size_t>(bits, 64);
    const std::size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    std::string filter(bytes + 1, '\0');
    auto* array = reinterpret_cast<unsigned char*>(filter.data());
    for (std::uint32_t h : hashes_) {
        const std::uint32_t delta = probe_delta(h);
        for (int i = 0; i < k_; ++i) {
            const std::size_t pos = h % bits;
            array[pos / 8] = static_cast<unsigned char>(array[pos / 8] | (1u << (pos % 8)));
            h += delta;
        }
    }
    filter[bytes] = static_cast<char>(k_);
    hashes_.clear();
    return filter;
}

bool bloom_may_match(std::string_view filter, std::string_view key) noexcept {
    if (filter.size() < 2) return true;
    const std::size_t bytes = filter.size() - 1;
    const std::size_t bits = bytes * 8;
    const int k = static_cast<unsigned char>(filter[bytes]);
    if (k < 1 || k > kMaxProbes) return true;  // reserved for future encodings

    const auto* array = reinterpret_cast<const unsigned char*>(filter.data());
    std::uint32_t h = hash32(key, kBloomSeed);
    const std::uint32_t delta = probe_delta(h);
    for (int i = 0; i < k; ++i) {
        const std::size_t pos = h % bits;
        if ((array[pos / 8] & (1u << (pos % 8))) == 0) return false;
        h += delta;
    }
    return true;
}

}  // namespace strata
