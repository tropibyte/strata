// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// A two-level iterator walks an index iterator whose values each name a
// sequence of entries (a data block of a table, or a table in a level), and
// yields the entries of those sequences in order.

#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "strata/iterator.hpp"

namespace strata {

/// Turns an index value into an iterator over the entries it names. May
/// return an iterator whose status() is an error.
using BlockFunction = std::function<std::unique_ptr<Iterator>(std::string_view index_value)>;

std::unique_ptr<Iterator> new_two_level_iterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function);

}  // namespace strata
