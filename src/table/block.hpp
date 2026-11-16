// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Block: a parsed, read-only block (see block_builder.hpp for the format),
// and the cache that holds them.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "strata/iterator.hpp"
#include "util/cache.hpp"

namespace strata {

class Comparator;

class Block {
public:
    /// Takes ownership of the block contents.
    explicit Block(std::string contents);

    std::size_t size() const noexcept { return data_.size(); }

    /// False if the restart array is malformed; iterators over such a block
    /// report Corruption.
    bool well_formed() const noexcept { return restart_offset_ != kMalformed; }

    std::uint32_t num_restarts() const noexcept { return num_restarts_; }

    /// An iterator over `block`, which it keeps alive.
    static std::unique_ptr<Iterator> new_iterator(std::shared_ptr<const Block> block, const Comparator* comparator);

private:
    friend class BlockIterator;
    static constexpr std::uint32_t kMalformed = 0xffffffffu;

    std::string data_;
    std::uint32_t restart_offset_ = kMalformed;  // where the restart array starts
    std::uint32_t num_restarts_ = 0;
};

/// Block cache key: (table cache id, block offset).
struct BlockCacheKey {
    std::uint64_t table_id;
    std::uint64_t offset;
    friend bool operator==(const BlockCacheKey& a, const BlockCacheKey& b) noexcept {
        return a.table_id == b.table_id && a.offset == b.offset;
    }
};

struct BlockCacheKeyHash {
    std::size_t operator()(const BlockCacheKey& k) const noexcept {
        return static_cast<std::size_t>(k.table_id * 0x9e3779b97f4a7c15ull ^ (k.offset + 0x632be59bd9b4e019ull));
    }
};

using BlockCache = LruCache<BlockCacheKey, Block, BlockCacheKeyHash>;

}  // namespace strata
