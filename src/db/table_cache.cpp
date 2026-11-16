// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/table_cache.hpp"

#include "db/filename.hpp"

namespace strata {

TableCache::TableCache(std::string dbname, Env* env, const TableOptions& table_options, int max_open_tables,
                       std::size_t block_cache_bytes)
    : dbname_(std::move(dbname)),
      env_(env),
      table_options_(table_options),
      block_cache_(block_cache_bytes > 0 ? std::make_unique<BlockCache>(block_cache_bytes) : nullptr),
      // Each table is charged 1, so the capacity is a count of tables. Few
      // shards: the count per shard must stay meaningful.
      tables_(static_cast<std::size_t>(max_open_tables > 0 ? max_open_tables : 1), 2) {}

Status TableCache::find_table(std::uint64_t number, std::uint64_t file_size, std::shared_ptr<Table>* out) {
    *out = tables_.lookup(number);
    if (*out) return Status::ok();

    const std::string name = table_file_name(dbname_, number);
    std::unique_ptr<RandomAccessFile> file;
    Status s = env_->new_random_access_file(name, &file);
    if (!s.is_ok()) return s;
    std::shared_ptr<Table> table;
    s = Table::open(table_options_, std::move(file), file_size, block_cache_.get(), &table);
    if (!s.is_ok()) return Status(s.code() == Status::Code::Corruption ? Status::corruption(name, s.message()) : s);
    // Errors are not cached: a transient failure should be retried later.
    tables_.insert(number, table, 1);
    *out = std::move(table);
    return Status::ok();
}

std::unique_ptr<Iterator> TableCache::new_iterator(const ReadOptions& options, std::uint64_t number,
                                                   std::uint64_t file_size) {
    std::shared_ptr<Table> table;
    Status s = find_table(number, file_size, &table);
    if (!s.is_ok()) return new_empty_iterator(s);
    return table->new_iterator(options);
}

Status TableCache::get(const ReadOptions& options, std::uint64_t number, std::uint64_t file_size, std::string_view key,
                       const std::function<void(std::string_view, std::string_view)>& handle) {
    std::shared_ptr<Table> table;
    Status s = find_table(number, file_size, &table);
    if (!s.is_ok()) return s;
    return table->internal_get(options, key, handle);
}

void TableCache::evict(std::uint64_t number) {
    tables_.erase(number);
}

}  // namespace strata
