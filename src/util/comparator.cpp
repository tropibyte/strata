// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "strata/comparator.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace strata {
namespace {

int bytewise_compare(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n != 0) {
        const int r = std::memcmp(a.data(), b.data(), n);
        if (r != 0) return r;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

class BytewiseComparator final : public Comparator {
public:
    int compare(std::string_view a, std::string_view b) const override { return bytewise_compare(a, b); }

    std::string_view name() const override { return "strata.BytewiseComparator"; }

    void find_shortest_separator(std::string& start, std::string_view limit) const override {
        // Length of the common prefix.
        const std::size_t min_len = std::min(start.size(), limit.size());
        std::size_t diff = 0;
        while (diff < min_len && start[diff] == limit[diff]) ++diff;
        if (diff >= min_len) return;  // one is a prefix of the other

        const auto byte = static_cast<std::uint8_t>(start[diff]);
        if (byte < 0xff && byte + 1 < static_cast<std::uint8_t>(limit[diff])) {
            start[diff] = static_cast<char>(byte + 1);
            start.resize(diff + 1);
        }
    }

    void find_short_successor(std::string& key) const override {
        // Increment the first byte that is not 0xff and truncate after it.
        for (std::size_t i = 0; i < key.size(); ++i) {
            const auto byte = static_cast<std::uint8_t>(key[i]);
            if (byte != 0xff) {
                key[i] = static_cast<char>(byte + 1);
                key.resize(i + 1);
                return;
            }
        }
        // All 0xff: leave it alone.
    }
};

class ReverseBytewiseComparator final : public Comparator {
public:
    int compare(std::string_view a, std::string_view b) const override { return -bytewise_compare(a, b); }
    std::string_view name() const override { return "strata.ReverseBytewiseComparator"; }
    void find_shortest_separator(std::string&, std::string_view) const override {}
    void find_short_successor(std::string&) const override {}
};

}  // namespace

const Comparator* bytewise_comparator() noexcept {
    static const BytewiseComparator instance;
    return &instance;
}

const Comparator* reverse_bytewise_comparator() noexcept {
    static const ReverseBytewiseComparator instance;
    return &instance;
}

}  // namespace strata
