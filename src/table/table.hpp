// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Table: an open, immutable table file. Thread-safe; held by shared_ptr so
// iterators can keep it open after it leaves the table cache.

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
#include "table/format.hpp"
#include "table/properties.hpp"
#include "table/table_options.hpp"

namespace strata {

class Table : public std::enable_shared_from_this<Table> {
public:
    /// Opens the table in `file`, which is `file_size` bytes long. Blocks are
    /// cached in `block_cache` if it is non-null (it must outlive the table).
    static Status open(const TableOptions& options, std::unique_ptr<RandomAccessFile> file, std::uint64_t file_size,
                       BlockCache* block_cache, std::shared_ptr<Table>* out);

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    ~Table();

    /// Iterator over every entry, in order.
    std::unique_ptr<Iterator> new_iterator(const ReadOptions& options) const;

    /// Finds the first entry with key >= `key` and calls handle(key, value)
    /// for it — unless the bloom filter shows the table has no entry whose
    /// filter key equals filter_key(`key`), in which case nothing is read.
    Status internal_get(const ReadOptions& options, std::string_view key,
                        const std::function<void(std::string_view, std::string_view)>& handle) const;

    /// Approximate file offset where `key` would be: the start of the data
    /// block that would hold it, or the end of the data for keys past the
    /// last one.
    std::uint64_t approximate_offset_of(std::string_view key) const;

    const TableProperties& properties() const noexcept { return props_; }
    bool has_filter() const noexcept { return !filter_.empty(); }
    std::uint64_t file_size() const noexcept { return file_size_; }

    /// Iterator over the index block (key -> encoded BlockHandle).
    std::unique_ptr<Iterator> new_index_iterator() const;

    /// Iterator over the data block named by an encoded BlockHandle.
    std::unique_ptr<Iterator> block_iterator(const ReadOptions& options, std::string_view handle_encoding) const;

private:
    Table(const TableOptions& options, std::unique_ptr<RandomAccessFile> file, std::uint64_t file_size,
          BlockCache* cache);

    Status read_meta(const Footer& footer);

    TableOptions options_;
    std::unique_ptr<RandomAccessFile> file_;
    std::uint64_t file_size_;
    BlockCache* cache_;
    std::uint64_t cache_id_ = 0;
    std::shared_ptr<const Block> index_block_;
    std::string filter_;
    TableProperties props_;
    BlockHandle metaindex_handle_;
};

}  // namespace strata
