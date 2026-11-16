// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Arena: bump allocator for memtable entries and skiplist nodes. Memory is
// released all at once when the arena is destroyed.
//
// allocate() is not thread-safe (the memtable has a single writer);
// memory_usage() may be read from any thread.

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace strata {

class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    /// Returns n bytes (n > 0) with no particular alignment.
    char* allocate(std::size_t n);

    /// Returns n bytes aligned for any fundamental type.
    char* allocate_aligned(std::size_t n);

    /// Bytes reserved from the system, including block overhead.
    std::size_t memory_usage() const noexcept { return memory_usage_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kBlockSize = 4096;

    char* allocate_fallback(std::size_t n);
    char* allocate_new_block(std::size_t n);

    char* ptr_ = nullptr;
    std::size_t remaining_ = 0;
    std::vector<std::unique_ptr<char[]>> blocks_;
    std::atomic<std::size_t> memory_usage_{0};
};

}  // namespace strata
