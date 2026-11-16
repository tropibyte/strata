// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Settings shared by the table builder and reader.

#pragma once

#include <cstddef>
#include <string_view>

#include "strata/options.hpp"

namespace strata {

class Comparator;

/// Maps a key stored in the table to the part the bloom filter covers. The
/// database stores internal keys (user key + sequence + type) but looks
/// them up by user key, so it passes extract_user_key here.
using FilterKeyFunction = std::string_view (*)(std::string_view table_key);

struct TableOptions {
    /// Orders the keys in the table. Required.
    const Comparator* comparator = nullptr;

    std::size_t block_size = 4096;
    int block_restart_interval = 16;

    /// 0 builds no filter.
    int bloom_bits_per_key = 10;

    Compression compression = Compression::Lz;

    /// Null: the filter covers whole keys.
    FilterKeyFunction filter_key = nullptr;
};

}  // namespace strata
