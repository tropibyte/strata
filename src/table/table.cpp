// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/table.hpp"

#include "strata/comparator.hpp"
#include "table/two_level_iterator.hpp"
#include "util/bloom.hpp"

namespace strata {

Table::Table(const TableOptions& options, std::unique_ptr<RandomAccessFile> file, std::uint64_t file_size,
             BlockCache* cache)
    : options_(options), file_(std::move(file)), file_size_(file_size), cache_(cache) {
    if (cache_ != nullptr) cache_id_ = cache_->new_id();
}

Table::~Table() = default;

Status Table::open(const TableOptions& options, std::unique_ptr<RandomAccessFile> file, std::uint64_t file_size,
                   BlockCache* block_cache, std::shared_ptr<Table>* out) {
    out->reset();
    if (file_size < Footer::kEncodedLength) return Status::corruption("file is too short to be a table");

    std::string footer_bytes;
    Status s = file->read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, footer_bytes);
    if (!s.is_ok()) return s;
    Footer footer;
    s = footer.decode_from(footer_bytes);
    if (!s.is_ok()) return s;
    if (footer.index.offset() + footer.index.size() + kBlockTrailerSize > file_size ||
        footer.metaindex.offset() + footer.metaindex.size() + kBlockTrailerSize > file_size) {
        return Status::corruption("table footer points past the end of the file");
    }

    std::string index_contents;
    s = read_block(*file, footer.index, /*verify=*/true, &index_contents);
    if (!s.is_ok()) return s;

    std::shared_ptr<Table> table(new Table(options, std::move(file), file_size, block_cache));
    table->index_block_ = std::make_shared<const Block>(std::move(index_contents));
    if (!table->index_block_->well_formed()) return Status::corruption("bad index block");
    table->metaindex_handle_ = footer.metaindex;
    s = table->read_meta(footer);
    if (!s.is_ok()) return s;
    *out = std::move(table);
    return Status::ok();
}

Status Table::read_meta(const Footer& footer) {
    std::string contents;
    Status s = read_block(*file_, footer.metaindex, /*verify=*/true, &contents);
    if (!s.is_ok()) return s;
    auto meta = std::make_shared<const Block>(std::move(contents));
    auto it = Block::new_iterator(meta, bytewise_comparator());
    for (it->seek_to_first(); it->valid(); it->next()) {
        std::string_view value = it->value();
        BlockHandle handle;
        s = handle.decode_from(value);
        if (!s.is_ok()) return s;
        if (it->key() == kFilterBlockName) {
            // A damaged filter is not fatal: lookups just go without it.
            if (!read_block(*file_, handle, true, &filter_).is_ok()) filter_.clear();
        } else if (it->key() == kPropertiesBlockName) {
            std::string props;
            s = read_block(*file_, handle, true, &props);
            if (s.is_ok()) s = TableProperties::decode(props, &props_);
            if (!s.is_ok()) return s;
        }
    }
    s = it->status();
    if (s.is_ok() && !props_.comparator.empty() && props_.comparator != options_.comparator->name()) {
        return Status::invalid_argument("table was written with comparator " + props_.comparator);
    }
    return s;
}

std::unique_ptr<Iterator> Table::new_index_iterator() const {
    return Block::new_iterator(index_block_, options_.comparator);
}

std::unique_ptr<Iterator> Table::block_iterator(const ReadOptions& options, std::string_view handle_encoding) const {
    BlockHandle handle;
    std::string_view input = handle_encoding;
    Status s = handle.decode_from(input);
    if (!s.is_ok()) return new_empty_iterator(s);
    if (handle.offset() + handle.size() + kBlockTrailerSize > file_size_) {
        return new_empty_iterator(Status::corruption("block handle points past the end of the table"));
    }

    std::shared_ptr<const Block> block;
    const BlockCacheKey key{cache_id_, handle.offset()};
    if (cache_ != nullptr) block = cache_->lookup(key);
    if (!block) {
        std::string contents;
        s = read_block(*file_, handle, options.verify_checksums, &contents);
        if (!s.is_ok()) return new_empty_iterator(s);
        auto fresh = std::make_shared<Block>(std::move(contents));
        if (cache_ != nullptr && options.fill_cache && fresh->well_formed()) {
            cache_->insert(key, fresh, fresh->size());
        }
        block = std::move(fresh);
    }
    return Block::new_iterator(std::move(block), options_.comparator);
}

std::unique_ptr<Iterator> Table::new_iterator(const ReadOptions& options) const {
    std::shared_ptr<const Table> self = shared_from_this();
    return new_two_level_iterator(new_index_iterator(), [self, options](std::string_view handle) {
        return self->block_iterator(options, handle);
    });
}

Status Table::internal_get(const ReadOptions& options, std::string_view key,
                           const std::function<void(std::string_view, std::string_view)>& handle) const {
    if (!filter_.empty()) {
        const std::string_view filter_key = options_.filter_key != nullptr ? options_.filter_key(key) : key;
        if (!bloom_may_match(filter_, filter_key)) return Status::ok();
    }
    auto index = new_index_iterator();
    index->seek(key);
    if (!index->valid()) return index->status();

    auto block = block_iterator(options, index->value());
    block->seek(key);
    if (block->valid()) handle(block->key(), block->value());
    return block->status();
}

std::uint64_t Table::approximate_offset_of(std::string_view key) const {
    auto index = new_index_iterator();
    index->seek(key);
    if (index->valid()) {
        BlockHandle handle;
        std::string_view input = index->value();
        if (handle.decode_from(input).is_ok()) return handle.offset();
    }
    // Past the last key, or a damaged index: the data ends where the
    // metaindex begins (close enough to the end of the data blocks).
    return metaindex_handle_.offset();
}

}  // namespace strata
