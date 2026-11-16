// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/properties.hpp"

#include <memory>

#include "strata/comparator.hpp"
#include "table/block.hpp"
#include "table/block_builder.hpp"

namespace strata {
namespace {

constexpr std::string_view kNumEntries = "table.num-entries";
constexpr std::string_view kNumDataBlocks = "table.num-data-blocks";
constexpr std::string_view kRawKeySize = "table.raw-key-size";
constexpr std::string_view kRawValueSize = "table.raw-value-size";
constexpr std::string_view kDataSize = "table.data-size";
constexpr std::string_view kIndexSize = "table.index-size";
constexpr std::string_view kFilterSize = "table.filter-size";
constexpr std::string_view kCompressedBlocks = "table.compressed-blocks";
constexpr std::string_view kComparator = "table.comparator";

bool parse_u64(std::string_view s, std::uint64_t* out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (~std::uint64_t{0} - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

}  // namespace

std::string TableProperties::encode() const {
    std::map<std::string, std::string> all = user;
    all[std::string(kNumEntries)] = std::to_string(num_entries);
    all[std::string(kNumDataBlocks)] = std::to_string(num_data_blocks);
    all[std::string(kRawKeySize)] = std::to_string(raw_key_size);
    all[std::string(kRawValueSize)] = std::to_string(raw_value_size);
    all[std::string(kDataSize)] = std::to_string(data_size);
    all[std::string(kIndexSize)] = std::to_string(index_size);
    all[std::string(kFilterSize)] = std::to_string(filter_size);
    all[std::string(kCompressedBlocks)] = std::to_string(compressed_blocks);
    all[std::string(kComparator)] = comparator;

    BlockBuilder builder(1, bytewise_comparator());
    for (const auto& [name, value] : all) builder.add(name, value);
    return std::string(builder.finish());
}

Status TableProperties::decode(std::string_view block_contents, TableProperties* out) {
    *out = TableProperties{};
    auto block = std::make_shared<const Block>(std::string(block_contents));
    auto it = Block::new_iterator(block, bytewise_comparator());
    for (it->seek_to_first(); it->valid(); it->next()) {
        const std::string_view name = it->key();
        const std::string_view value = it->value();
        std::uint64_t* field = nullptr;
        if (name == kNumEntries)
            field = &out->num_entries;
        else if (name == kNumDataBlocks)
            field = &out->num_data_blocks;
        else if (name == kRawKeySize)
            field = &out->raw_key_size;
        else if (name == kRawValueSize)
            field = &out->raw_value_size;
        else if (name == kDataSize)
            field = &out->data_size;
        else if (name == kIndexSize)
            field = &out->index_size;
        else if (name == kFilterSize)
            field = &out->filter_size;
        else if (name == kCompressedBlocks)
            field = &out->compressed_blocks;

        if (field != nullptr) {
            if (!parse_u64(value, field)) return Status::corruption("bad table property", name);
        } else if (name == kComparator) {
            out->comparator.assign(value);
        } else {
            out->user.emplace(std::string(name), std::string(value));
        }
    }
    return it->status();
}

}  // namespace strata
