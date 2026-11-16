// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// TableCache: keeps recently used table files open, and owns the block cache
// they share. Thread-safe.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "strata/env.hpp"
#include "strata/iterator.hpp"
#include "strata/options.hpp"
#include "table/block.hpp"
#include "table/table.hpp"
#include "table/table_options.hpp"
#include "util/cache.hpp"

namespace strata {

class TableCache {
public:
    TableCache(std::string dbname, Env* env, const TableOptions& table_options, int max_open_tables,
               std::size_t block_cache_bytes);

    TableCache(const TableCache&) = delete;
    TableCache& operator=(const TableCache&) = delete;

    /// Iterator over table file `number` (of `file_size` bytes). On failure,
    /// an empty iterator carrying the error.
    std::unique_ptr<Iterator> new_iterator(const ReadOptions& options, std::uint64_t number, std::uint64_t file_size);

    /// Point lookup in one table; see Table::internal_get.
    Status get(const ReadOptions& options, std::uint64_t number, std::uint64_t file_size, std::string_view key,
               const std::function<void(std::string_view, std::string_view)>& handle);

    /// Opens (or finds) the table.
    Status find_table(std::uint64_t number, std::uint64_t file_size, std::shared_ptr<Table>* out);

    /// Drops the table from the cache (it is about to be deleted).
    void evict(std::uint64_t number);

    BlockCache* block_cache() noexcept { return block_cache_.get(); }
    const TableOptions& table_options() const noexcept { return table_options_; }

    std::size_t open_tables() const { return tables_.size(); }

private:
    std::string dbname_;
    Env* env_;
    TableOptions table_options_;
    std::unique_ptr<BlockCache> block_cache_;
    LruCache<std::uint64_t, Table> tables_;
};

}  // namespace strata
