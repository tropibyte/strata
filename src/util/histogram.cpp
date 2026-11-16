// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace strata {

const std::array<double, Histogram::kNumBuckets>& Histogram::bucket_limits() {
    // Upper bounds: 1, 2, 3, 4, 5, 6, 8, 10, then roughly x1.3 per bucket up
    // to ~1e8; the last bucket catches everything else.
    static const std::array<double, kNumBuckets> limits = [] {
        std::array<double, kNumBuckets> l{};
        double v = 1;
        for (std::size_t i = 0; i + 1 < kNumBuckets; ++i) {
            l[i] = std::round(v);
            double next = v * 1.32;
            if (std::round(next) <= l[i]) next = l[i] + 1;
            v = next;
        }
        l[kNumBuckets - 1] = std::numeric_limits<double>::max();
        return l;
    }();
    return limits;
}

void Histogram::clear() {
    min_ = std::numeric_limits<double>::max();
    max_ = 0;
    count_ = 0;
    sum_ = 0;
    sum_squares_ = 0;
    buckets_.fill(0);
}

void Histogram::add(double value) {
    const auto& limits = bucket_limits();
    const auto it = std::lower_bound(limits.begin(), limits.end(), value);
    const auto b = static_cast<std::size_t>(it - limits.begin());
    buckets_[std::min(b, kNumBuckets - 1)] += 1;
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
    ++count_;
    sum_ += value;
    sum_squares_ += value * value;
}

void Histogram::merge(const Histogram& other) {
    if (other.count_ == 0) return;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    count_ += other.count_;
    sum_ += other.sum_;
    sum_squares_ += other.sum_squares_;
    for (std::size_t i = 0; i < kNumBuckets; ++i) buckets_[i] += other.buckets_[i];
}

double Histogram::average() const noexcept {
    return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_);
}

double Histogram::standard_deviation() const noexcept {
    if (count_ == 0) return 0.0;
    const double n = static_cast<double>(count_);
    const double variance = (sum_squares_ * n - sum_ * sum_) / (n * n);
    return variance <= 0 ? 0.0 : std::sqrt(variance);
}

double Histogram::percentile(double p) const noexcept {
    if (count_ == 0) return 0.0;
    const auto& limits = bucket_limits();
    const double threshold = static_cast<double>(count_) * (p / 100.0);
    double cumulative = 0;
    for (std::size_t b = 0; b < kNumBuckets; ++b) {
        cumulative += static_cast<double>(buckets_[b]);
        if (cumulative >= threshold) {
            // Interpolate within the bucket.
            const double left_point = b == 0 ? 0.0 : limits[b - 1];
            const double right_point = b + 1 == kNumBuckets ? max_ : limits[b];
            const double left_sum = cumulative - static_cast<double>(buckets_[b]);
            const double pos = buckets_[b] == 0 ? 0.0 : (threshold - left_sum) / static_cast<double>(buckets_[b]);
            double r = left_point + (right_point - left_point) * pos;
            r = std::clamp(r, min_, max_);
            return r;
        }
    }
    return max_;
}

std::string Histogram::to_string() const {
    std::string out;
    char buf[200];
    std::snprintf(buf, sizeof(buf), "Count: %llu  Average: %.4f  StdDev: %.2f\n",
                  static_cast<unsigned long long>(count_), average(), standard_deviation());
    out.append(buf);
    std::snprintf(buf, sizeof(buf), "Min: %.4f  Median: %.4f  P99: %.4f  Max: %.4f\n", min(), median(),
                  percentile(99.0), max());
    out.append(buf);
    if (count_ == 0) return out;
    out.append("------------------------------------------------------\n");
    const auto& limits = bucket_limits();
    const double mult = 100.0 / static_cast<double>(count_);
    double cumulative = 0;
    for (std::size_t b = 0; b < kNumBuckets; ++b) {
        if (buckets_[b] == 0) continue;
        cumulative += static_cast<double>(buckets_[b]);
        const double left = b == 0 ? 0.0 : limits[b - 1];
        const double right = b + 1 == kNumBuckets ? max_ : limits[b];
        std::snprintf(buf, sizeof(buf), "[ %8.0f, %8.0f ) %8llu %7.3f%% %7.3f%% ", left, right,
                      static_cast<unsigned long long>(buckets_[b]), mult * static_cast<double>(buckets_[b]),
                      mult * cumulative);
        out.append(buf);
        const auto marks =
            static_cast<std::size_t>(20.0 * static_cast<double>(buckets_[b]) / static_cast<double>(count_) + 0.5);
        out.append(marks, '#');
        out.push_back('\n');
    }
    return out;
}

}  // namespace strata
