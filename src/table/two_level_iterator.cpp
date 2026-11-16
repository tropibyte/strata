// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/two_level_iterator.hpp"

#include <string>

#include "table/iterator_wrapper.hpp"

namespace strata {
namespace {

class TwoLevelIterator final : public Iterator {
public:
    TwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function)
        : block_function_(std::move(block_function)), index_(std::move(index_iter)) {}

    void seek(std::string_view target) override {
        index_.seek(target);
        init_data_block();
        if (data_.iter() != nullptr) data_.seek(target);
        skip_empty_blocks_forward();
    }

    void seek_to_first() override {
        index_.seek_to_first();
        init_data_block();
        if (data_.iter() != nullptr) data_.seek_to_first();
        skip_empty_blocks_forward();
    }

    void seek_to_last() override {
        index_.seek_to_last();
        init_data_block();
        if (data_.iter() != nullptr) data_.seek_to_last();
        skip_empty_blocks_backward();
    }

    void next() override {
        data_.next();
        skip_empty_blocks_forward();
    }

    void prev() override {
        data_.prev();
        skip_empty_blocks_backward();
    }

    bool valid() const override { return data_.valid(); }
    std::string_view key() const override { return data_.key(); }
    std::string_view value() const override { return data_.value(); }

    Status status() const override {
        if (index_.iter() != nullptr) {
            Status s = index_.status();
            if (!s.is_ok()) return s;
        }
        if (data_.iter() != nullptr) {
            Status s = data_.status();
            if (!s.is_ok()) return s;
        }
        return status_;
    }

private:
    void save_error(const Status& s) {
        if (status_.is_ok() && !s.is_ok()) status_ = s;
    }

    void set_data_iterator(std::unique_ptr<Iterator> iter) {
        if (data_.iter() != nullptr) save_error(data_.status());
        data_.set(std::move(iter));
    }

    void init_data_block() {
        if (!index_.valid()) {
            set_data_iterator(nullptr);
            data_block_handle_.clear();
            return;
        }
        const std::string_view handle = index_.value();
        if (data_.iter() != nullptr && handle == data_block_handle_) {
            return;  // already positioned in this block
        }
        set_data_iterator(block_function_(handle));
        data_block_handle_.assign(handle);
    }

    void skip_empty_blocks_forward() {
        while (data_.iter() == nullptr || !data_.valid()) {
            if (!index_.valid()) {
                set_data_iterator(nullptr);
                return;
            }
            index_.next();
            init_data_block();
            if (data_.iter() != nullptr) data_.seek_to_first();
        }
    }

    void skip_empty_blocks_backward() {
        while (data_.iter() == nullptr || !data_.valid()) {
            if (!index_.valid()) {
                set_data_iterator(nullptr);
                return;
            }
            index_.prev();
            init_data_block();
            if (data_.iter() != nullptr) data_.seek_to_last();
        }
    }

    BlockFunction block_function_;
    IteratorWrapper index_;
    IteratorWrapper data_;
    std::string data_block_handle_;  // index value data_ was built from
    Status status_;
};

}  // namespace

std::unique_ptr<Iterator> new_two_level_iterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function) {
    return std::make_unique<TwoLevelIterator>(std::move(index_iter), std::move(block_function));
}

}  // namespace strata
