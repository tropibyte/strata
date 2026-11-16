// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/block_builder.hpp"

#include <algorithm>
#include <cassert>

#include "strata/comparator.hpp"
#include "util/coding.hpp"

namespace strata {

BlockBuilder::BlockBuilder(int restart_interval, const Comparator* comparator)
    : restart_interval_(std::max(1, restart_interval)), comparator_(comparator) {
    restarts_.push_back(0);
}

void BlockBuilder::reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_ = 0;
    entries_ = 0;
    finished_ = false;
    last_key_.clear();
}

std::size_t BlockBuilder::current_size_estimate() const {
    return buffer_.size() + restarts_.size() * sizeof(std::uint32_t) + sizeof(std::uint32_t);
}

std::string_view BlockBuilder::finish() {
    for (std::uint32_t r : restarts_) put_fixed32(buffer_, r);
    put_fixed32(buffer_, static_cast<std::uint32_t>(restarts_.size()));
    finished_ = true;
    return buffer_;
}

void BlockBuilder::add(std::string_view key, std::string_view value) {
    assert(!finished_);
    assert(entries_ == 0 || comparator_ == nullptr || comparator_->compare(key, last_key_) > 0);

    std::size_t shared = 0;
    if (counter_ < restart_interval_) {
        const std::size_t limit = std::min(last_key_.size(), key.size());
        while (shared < limit && last_key_[shared] == key[shared]) ++shared;
    } else {
        restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
        counter_ = 0;
    }
    const std::size_t unshared = key.size() - shared;

    put_varint32(buffer_, static_cast<std::uint32_t>(shared));
    put_varint32(buffer_, static_cast<std::uint32_t>(unshared));
    put_varint32(buffer_, static_cast<std::uint32_t>(value.size()));
    buffer_.append(key.substr(shared));
    buffer_.append(value);

    last_key_.resize(shared);
    last_key_.append(key.substr(shared));
    ++counter_;
    ++entries_;
}

}  // namespace strata
