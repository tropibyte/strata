// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// IteratorWrapper: owns an iterator and caches valid() and key(), which
// merging and two-level iterators ask for far more often than they move.

#pragma once

#include <cassert>
#include <memory>
#include <string_view>

#include "strata/iterator.hpp"

namespace strata {

class IteratorWrapper {
public:
    IteratorWrapper() = default;
    explicit IteratorWrapper(std::unique_ptr<Iterator> iter) { set(std::move(iter)); }

    Iterator* iter() const noexcept { return iter_.get(); }

    void set(std::unique_ptr<Iterator> iter) {
        iter_ = std::move(iter);
        if (iter_) {
            update();
        } else {
            valid_ = false;
        }
    }

    bool valid() const noexcept { return valid_; }
    std::string_view key() const noexcept {
        assert(valid_);
        return key_;
    }
    std::string_view value() const {
        assert(valid_);
        return iter_->value();
    }
    Status status() const {
        assert(iter_);
        return iter_->status();
    }

    void next() {
        iter_->next();
        update();
    }
    void prev() {
        iter_->prev();
        update();
    }
    void seek(std::string_view target) {
        iter_->seek(target);
        update();
    }
    void seek_to_first() {
        iter_->seek_to_first();
        update();
    }
    void seek_to_last() {
        iter_->seek_to_last();
        update();
    }

private:
    void update() {
        valid_ = iter_->valid();
        if (valid_) key_ = iter_->key();
    }

    std::unique_ptr<Iterator> iter_;
    bool valid_ = false;
    std::string_view key_;
};

}  // namespace strata
