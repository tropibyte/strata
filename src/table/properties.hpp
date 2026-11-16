// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Table properties: facts about a table recorded in its properties block.
// Well-known numeric properties are fields; anything else the writer set
// (the database adds its own, prefixed "strata.") is kept in `user`.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "strata/status.hpp"

namespace strata {

struct TableProperties {
    std::uint64_t num_entries = 0;
    std::uint64_t num_data_blocks = 0;
    std::uint64_t raw_key_size = 0;    // sum of key lengths
    std::uint64_t raw_value_size = 0;  // sum of value lengths
    std::uint64_t data_size = 0;       // bytes of data blocks, as stored
    std::uint64_t index_size = 0;
    std::uint64_t filter_size = 0;
    std::uint64_t compressed_blocks = 0;  // data blocks stored compressed
    std::string comparator;
    std::map<std::string, std::string> user;

    /// Encodes as the contents of a properties block (sorted name -> value).
    std::string encode() const;

    /// Parses properties block contents.
    static Status decode(std::string_view block_contents, TableProperties* out);
};

}  // namespace strata
