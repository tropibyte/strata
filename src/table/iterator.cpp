// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "strata/iterator.hpp"

namespace strata {
namespace {

class EmptyIterator final : public Iterator {
public:
    explicit EmptyIterator(Status s) : status_(std::move(s)) {}
    bool valid() const override { return false; }
    void seek(std::string_view) override {}
    void seek_to_first() override {}
    void seek_to_last() override {}
    void next() override {}
    void prev() override {}
    std::string_view key() const override { return {}; }
    std::string_view value() const override { return {}; }
    Status status() const override { return status_; }

private:
    Status status_;
};

}  // namespace

std::unique_ptr<Iterator> new_empty_iterator(Status status) {
    return std::make_unique<EmptyIterator>(std::move(status));
}

}  // namespace strata
