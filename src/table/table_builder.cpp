// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/table_builder.hpp"

#include <cassert>

#include "strata/comparator.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata {

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval, options.comparator),
      index_block_(1, options.comparator),
      filter_(options.bloom_bits_per_key > 0 ? options.bloom_bits_per_key : 1) {
    assert(options_.comparator != nullptr);
    props_.comparator.assign(options_.comparator->name());
}

TableBuilder::~TableBuilder() = default;

void TableBuilder::add(std::string_view key, std::string_view value) {
    assert(!closed_);
    if (!status_.is_ok()) return;
    assert(props_.num_entries == 0 || options_.comparator->compare(key, last_key_) > 0);

    if (pending_index_entry_) {
        assert(data_block_.empty());
        options_.comparator->find_shortest_separator(last_key_, key);
        std::string handle_encoding;
        pending_handle_.encode_to(handle_encoding);
        index_block_.add(last_key_, handle_encoding);
        pending_index_entry_ = false;
    }

    if (options_.bloom_bits_per_key > 0) {
        filter_.add_key(options_.filter_key != nullptr ? options_.filter_key(key) : key);
    }

    last_key_.assign(key);
    props_.num_entries += 1;
    props_.raw_key_size += key.size();
    props_.raw_value_size += value.size();
    data_block_.add(key, value);

    if (data_block_.current_size_estimate() >= options_.block_size) flush_block();
}

void TableBuilder::flush_block() {
    assert(!closed_);
    if (!status_.is_ok() || data_block_.empty()) return;
    assert(!pending_index_entry_);
    write_block(data_block_, &pending_handle_, /*compress=*/true);
    if (status_.is_ok()) {
        pending_index_entry_ = true;
        props_.num_data_blocks += 1;
        status_ = file_->flush();
    }
}

void TableBuilder::write_block(BlockBuilder& block, BlockHandle* handle, bool compress) {
    const std::string_view raw = block.finish();
    const Compression type = compress_block(raw, compress ? options_.compression : Compression::None, &compressed_);
    if (compress && type != Compression::None) props_.compressed_blocks += 1;
    write_raw_block(compressed_, type, handle);
    compressed_.clear();
    block.reset();
}

void TableBuilder::write_raw_block(std::string_view contents, Compression type, BlockHandle* handle) {
    handle->set_offset(offset_);
    handle->set_size(contents.size());
    status_ = file_->append(contents);
    if (!status_.is_ok()) return;

    char trailer[kBlockTrailerSize];
    trailer[0] = static_cast<char>(type);
    std::uint32_t crc = crc32c::value(contents);
    crc = crc32c::extend(crc, trailer, 1);  // cover the type byte too
    encode_fixed32(trailer + 1, crc32c::mask(crc));
    status_ = file_->append(std::string_view(trailer, kBlockTrailerSize));
    if (status_.is_ok()) offset_ += contents.size() + kBlockTrailerSize;
}

void TableBuilder::set_property(const std::string& name, std::string value) {
    props_.user[name] = std::move(value);
}

Status TableBuilder::finish() {
    flush_block();
    assert(!closed_);
    closed_ = true;
    if (!status_.is_ok()) return status_;

    props_.data_size = offset_;

    // Filter block.
    BlockHandle filter_handle;
    bool have_filter = false;
    if (options_.bloom_bits_per_key > 0 && filter_.num_keys() > 0) {
        const std::string filter = filter_.finish();
        write_raw_block(filter, Compression::None, &filter_handle);
        props_.filter_size = filter.size();
        have_filter = true;
    }
    if (!status_.is_ok()) return status_;

    // Index block. Its size goes into the properties, so encode it first and
    // write it after the properties block.
    if (pending_index_entry_) {
        options_.comparator->find_short_successor(last_key_);
        std::string handle_encoding;
        pending_handle_.encode_to(handle_encoding);
        index_block_.add(last_key_, handle_encoding);
        pending_index_entry_ = false;
    }
    const std::string index_contents(index_block_.finish());
    props_.index_size = index_contents.size();

    // Properties block.
    BlockHandle props_handle;
    write_raw_block(props_.encode(), Compression::None, &props_handle);
    if (!status_.is_ok()) return status_;

    // Metaindex block: sorted by name.
    BlockBuilder meta(1, bytewise_comparator());
    std::string handle_encoding;
    if (have_filter) {
        filter_handle.encode_to(handle_encoding);
        meta.add(kFilterBlockName, handle_encoding);
    }
    handle_encoding.clear();
    props_handle.encode_to(handle_encoding);
    meta.add(kPropertiesBlockName, handle_encoding);
    BlockHandle metaindex_handle;
    write_block(meta, &metaindex_handle, /*compress=*/false);
    if (!status_.is_ok()) return status_;

    BlockHandle index_handle;
    write_raw_block(index_contents, Compression::None, &index_handle);
    if (!status_.is_ok()) return status_;

    Footer footer;
    footer.metaindex = metaindex_handle;
    footer.index = index_handle;
    std::string footer_encoding;
    footer.encode_to(footer_encoding);
    status_ = file_->append(footer_encoding);
    if (status_.is_ok()) offset_ += footer_encoding.size();
    return status_;
}

void TableBuilder::abandon() {
    assert(!closed_);
    closed_ = true;
}

}  // namespace strata
