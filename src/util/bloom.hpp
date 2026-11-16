// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Bloom filters. Each table file carries one filter over the user keys of
// all its entries, so a point lookup can skip a table that cannot hold the
// key without reading any data block.
//
// Filter layout: ceil(bits/8) bytes of bit array, then one byte holding the
// number of probes k. Probe i tests bit (h + i*delta) mod bits, where h is
// hash32(key) and delta is h rotated right by 15 bits (double hashing).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

class BloomFilterBuilder {
public:
    /// bits_per_key must be at least 1.
    explicit BloomFilterBuilder(int bits_per_key);

    void add_key(std::string_view key);

    std::size_t num_keys() const noexcept { return hashes_.size(); }

    /// The filter over every key added so far. Resets the builder.
    std::string finish();

    /// Probes per key used for this bits_per_key.
    int num_probes() const noexcept { return k_; }

private:
    int bits_per_key_;
    int k_;
    std::vector<std::uint32_t> hashes_;
};

/// False means `key` was certainly not added to `filter`. A malformed filter
/// matches everything.
bool bloom_may_match(std::string_view filter, std::string_view key) noexcept;

}  // namespace strata
