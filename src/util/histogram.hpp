// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Histogram of non-negative samples (latencies in microseconds, sizes) in
// exponentially growing buckets, with approximate percentiles.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace strata {

class Histogram {
public:
    Histogram() { clear(); }

    void clear();
    void add(double value);
    void merge(const Histogram& other);

    std::uint64_t count() const noexcept { return count_; }
    double min() const noexcept { return count_ == 0 ? 0.0 : min_; }
    double max() const noexcept { return count_ == 0 ? 0.0 : max_; }
    double average() const noexcept;
    double standard_deviation() const noexcept;

    /// Approximate p-th percentile, p in [0, 100].
    double percentile(double p) const noexcept;
    double median() const noexcept { return percentile(50.0); }

    /// Multi-line summary with a bar for each non-empty bucket.
    std::string to_string() const;

private:
    static constexpr std::size_t kNumBuckets = 64;
    static const std::array<double, kNumBuckets>& bucket_limits();

    double min_ = 0;
    double max_ = 0;
    std::uint64_t count_ = 0;
    double sum_ = 0;
    double sum_squares_ = 0;
    std::array<std::uint64_t, kNumBuckets> buckets_{};
};

}  // namespace strata
