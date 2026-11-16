// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// A merging iterator yields the union of its children in comparator order.
// It does not drop duplicates: when several children hold equal keys, all of
// them are produced (in an unspecified order among themselves).

#pragma once

#include <memory>
#include <vector>

#include "strata/iterator.hpp"

namespace strata {

class Comparator;

/// Takes ownership of the children. With zero children returns an empty
/// iterator; with one, returns that child.
std::unique_ptr<Iterator> new_merging_iterator(const Comparator* comparator,
                                               std::vector<std::unique_ptr<Iterator>> children);

}  // namespace strata
