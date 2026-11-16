// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// DBIter turns an iterator over internal keys (a merge of the memtables and
// table files) into the user-facing iterator: for each user key it yields
// only the newest entry visible at the snapshot sequence, and skips keys
// whose newest visible entry is a deletion.

#pragma once

#include <memory>
#include <vector>

#include "db/dbformat.hpp"
#include "strata/iterator.hpp"

namespace strata {

/// `pins` are kept alive for the iterator's lifetime (the memtables and
/// version the internal iterator reads from).
std::unique_ptr<Iterator> new_db_iterator(const Comparator* user_comparator, std::unique_ptr<Iterator> internal,
                                          SequenceNumber sequence, std::vector<std::shared_ptr<const void>> pins);

}  // namespace strata
