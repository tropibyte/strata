// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/arena.hpp"

#include <cassert>
#include <cstdint>

namespace strata {

char* Arena::allocate(std::size_t n) {
    assert(n > 0);
    if (n <= remaining_) {
        char* result = ptr_;
        ptr_ += n;
        remaining_ -= n;
        return result;
    }
    return allocate_fallback(n);
}

char* Arena::allocate_aligned(std::size_t n) {
    constexpr std::size_t align = alignof(std::max_align_t);
    static_assert((align & (align - 1)) == 0, "alignment must be a power of two");
    const auto addr = reinterpret_cast<std::uintptr_t>(ptr_);
    const std::size_t mod = addr & (align - 1);
    const std::size_t slop = mod == 0 ? 0 : align - mod;
    const std::size_t needed = n + slop;
    char* result;
    if (needed <= remaining_) {
        result = ptr_ + slop;
        ptr_ += needed;
        remaining_ -= needed;
    } else {
        // New blocks come from operator new[], which is suitably aligned.
        result = allocate_fallback(n);
    }
    assert((reinterpret_cast<std::uintptr_t>(result) & (align - 1)) == 0);
    return result;
}

char* Arena::allocate_fallback(std::size_t n) {
    if (n > kBlockSize / 4) {
        // Large request: give it its own block so the current block's
        // remainder is not wasted.
        return allocate_new_block(n);
    }
    ptr_ = allocate_new_block(kBlockSize);
    remaining_ = kBlockSize;
    char* result = ptr_;
    ptr_ += n;
    remaining_ -= n;
    return result;
}

char* Arena::allocate_new_block(std::size_t n) {
    blocks_.push_back(std::unique_ptr<char[]>(new char[n]));
    memory_usage_.fetch_add(n + sizeof(char*), std::memory_order_relaxed);
    return blocks_.back().get();
}

}  // namespace strata
