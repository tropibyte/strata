// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// TableBuilder: writes a sorted run of key/value pairs as a table file (see
// format.hpp for the layout).
//
//     TableBuilder builder(options, file.get());
//     for (...) builder.add(key, value);   // strictly increasing keys
//     Status s = builder.finish();
//
// Not thread-safe.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "strata/env.hpp"
#include "table/block_builder.hpp"
#include "table/format.hpp"
#include "table/properties.hpp"
#include "table/table_options.hpp"
#include "util/bloom.hpp"

namespace strata {

class TableBuilder {
public:
    /// Does not take ownership of `file`; the caller syncs and closes it
    /// after finish().
    TableBuilder(const TableOptions& options, WritableFile* file);
    ~TableBuilder();

    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    /// Keys must be strictly increasing in the comparator's order.
    void add(std::string_view key, std::string_view value);

    /// Ends the current data block early, so the next key starts a new one.
    void flush_block();

    /// Records a property in the properties block. Call before finish().
    void set_property(const std::string& name, std::string value);

    /// The first error so far; once set, further adds are ignored.
    Status status() const { return status_; }

    /// Writes the filter, properties, index and footer.
    Status finish();

    /// Stops building; the file contents are unusable.
    void abandon();

    std::uint64_t num_entries() const noexcept { return props_.num_entries; }

    /// Bytes written so far (the final file size after finish()).
    std::uint64_t file_size() const noexcept { return offset_; }

    const TableProperties& properties() const noexcept { return props_; }

private:
    void write_block(BlockBuilder& block, BlockHandle* handle, bool compress);
    void write_raw_block(std::string_view contents, Compression type, BlockHandle* handle);

    TableOptions options_;
    WritableFile* file_;
    std::uint64_t offset_ = 0;
    Status status_;
    BlockBuilder data_block_;
    BlockBuilder index_block_;
    std::string last_key_;
    bool closed_ = false;

    // After a data block is written its index entry waits for the next key,
    // so the separator stored in the index can be shortened to sit between
    // the two blocks.
    bool pending_index_entry_ = false;
    BlockHandle pending_handle_;

    BloomFilterBuilder filter_;
    TableProperties props_;
    std::string compressed_;  // scratch
};

}  // namespace strata
