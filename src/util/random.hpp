// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Small, fast, seedable pseudo-random generator (SplitMix64). Deterministic
// across platforms, unlike the distributions in <random>, so tests and
// benchmarks reproduce exactly.

#pragma once

#include <cstdint>
#include <string>

namespace strata {

class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    /// Uniform in [0, n). Requires n > 0.
    std::uint64_t uniform(std::uint64_t n) noexcept { return next() % n; }

    /// True with probability 1/n.
    bool one_in(std::uint64_t n) noexcept { return uniform(n) == 0; }

    /// Picks a "bits" in [0, max_log] uniformly, then a number in [0, 2^bits).
    /// Favors small numbers, which is useful for sizes in tests.
    std::uint64_t skewed(int max_log) noexcept {
        const auto bits = static_cast<int>(uniform(static_cast<std::uint64_t>(max_log) + 1));
        return uniform(std::uint64_t{1} << bits);
    }

    /// A string of `len` printable characters.
    std::string printable(std::size_t len) {
        std::string s(len, ' ');
        for (auto& c : s) c = static_cast<char>(' ' + uniform(95));
        return s;
    }

private:
    std::uint64_t state_;
};

}  // namespace strata
