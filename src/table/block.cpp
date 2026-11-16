// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/block.hpp"

#include <cassert>

#include "strata/comparator.hpp"
#include "util/coding.hpp"

namespace strata {

Block::Block(std::string contents) : data_(std::move(contents)) {
    if (data_.size() < sizeof(std::uint32_t) || data_.size() > 0xffffffffu) return;
    const std::uint32_t n = decode_fixed32(data_.data() + data_.size() - sizeof(std::uint32_t));
    const std::size_t max_restarts = (data_.size() - sizeof(std::uint32_t)) / sizeof(std::uint32_t);
    if (n == 0 || n > max_restarts) return;
    num_restarts_ = n;
    restart_offset_ = static_cast<std::uint32_t>(data_.size() - (1 + std::size_t{n}) * sizeof(std::uint32_t));
}

class BlockIterator final : public Iterator {
public:
    BlockIterator(std::shared_ptr<const Block> block, const Comparator* comparator)
        : block_(std::move(block)),
          comparator_(comparator),
          data_(block_->data_.data()),
          restarts_(block_->restart_offset_),
          num_restarts_(block_->num_restarts_),
          current_(block_->restart_offset_),
          restart_index_(block_->num_restarts_) {
        if (!block_->well_formed()) {
            status_ = Status::corruption("bad block contents");
            restarts_ = 0;
            current_ = 0;
            num_restarts_ = 0;
            restart_index_ = 0;
        }
    }

    bool valid() const override { return current_ < restarts_; }
    Status status() const override { return status_; }

    std::string_view key() const override {
        assert(valid());
        return key_;
    }

    std::string_view value() const override {
        assert(valid());
        return value_;
    }

    void next() override {
        assert(valid());
        parse_next_entry();
    }

    void prev() override {
        assert(valid());
        // Back up to a restart point that starts before the current entry.
        const std::uint32_t original = current_;
        while (restart_point(restart_index_) >= original) {
            if (restart_index_ == 0) {
                // No entry before this one.
                current_ = restarts_;
                restart_index_ = num_restarts_;
                return;
            }
            --restart_index_;
        }
        seek_to_restart(restart_index_);
        // Walk forward to the entry just before `original`.
        while (parse_next_entry() && next_entry_offset() < original) {
        }
    }

    void seek(std::string_view target) override {
        if (num_restarts_ == 0) return;
        // Binary search for the last restart point whose key is < target.
        std::uint32_t left = 0;
        std::uint32_t right = num_restarts_ - 1;
        while (left < right) {
            const std::uint32_t mid = left + (right - left + 1) / 2;
            std::string_view mid_key;
            if (!restart_key(mid, &mid_key)) return;
            if (comparator_->compare(mid_key, target) < 0) {
                left = mid;  // keys before mid are all < target too
            } else {
                right = mid - 1;
            }
        }
        seek_to_restart(left);
        // Linear scan to the first key >= target.
        while (parse_next_entry()) {
            if (comparator_->compare(key_, target) >= 0) return;
        }
    }

    void seek_to_first() override {
        if (num_restarts_ == 0) return;
        seek_to_restart(0);
        parse_next_entry();
    }

    void seek_to_last() override {
        if (num_restarts_ == 0) return;
        seek_to_restart(num_restarts_ - 1);
        while (parse_next_entry() && next_entry_offset() < restarts_) {
        }
    }

private:
    std::uint32_t restart_point(std::uint32_t index) const {
        return decode_fixed32(data_ + restarts_ + index * sizeof(std::uint32_t));
    }

    std::uint32_t next_entry_offset() const {
        return static_cast<std::uint32_t>((value_.data() + value_.size()) - data_);
    }

    void seek_to_restart(std::uint32_t index) {
        key_.clear();
        restart_index_ = index;
        // parse_next_entry() starts at next_entry_offset(), so point the
        // (empty) value at the restart point.
        const std::uint32_t offset = restart_point(index);
        value_ = std::string_view(data_ + std::min(offset, restarts_), 0);
    }

    // Decodes the entry header at `p`; returns the start of the key delta or
    // nullptr if malformed.
    const char* decode_entry(const char* p, const char* limit, std::uint32_t* shared, std::uint32_t* unshared,
                             std::uint32_t* value_len) const {
        if (limit - p < 3) return nullptr;
        p = decode_varint32(p, limit, shared);
        if (p == nullptr) return nullptr;
        p = decode_varint32(p, limit, unshared);
        if (p == nullptr) return nullptr;
        p = decode_varint32(p, limit, value_len);
        if (p == nullptr) return nullptr;
        if (static_cast<std::uint64_t>(limit - p) < std::uint64_t{*unshared} + *value_len) return nullptr;
        return p;
    }

    bool restart_key(std::uint32_t index, std::string_view* key) {
        const std::uint32_t offset = restart_point(index);
        if (offset >= restarts_) {
            corrupted();
            return false;
        }
        std::uint32_t shared = 0, unshared = 0, value_len = 0;
        const char* p = decode_entry(data_ + offset, data_ + restarts_, &shared, &unshared, &value_len);
        if (p == nullptr || shared != 0) {
            corrupted();
            return false;
        }
        *key = std::string_view(p, unshared);
        return true;
    }

    bool parse_next_entry() {
        current_ = next_entry_offset();
        const char* p = data_ + current_;
        const char* limit = data_ + restarts_;
        if (p >= limit) {
            // Past the last entry.
            current_ = restarts_;
            restart_index_ = num_restarts_;
            return false;
        }
        std::uint32_t shared = 0, unshared = 0, value_len = 0;
        p = decode_entry(p, limit, &shared, &unshared, &value_len);
        if (p == nullptr || key_.size() < shared) {
            corrupted();
            return false;
        }
        key_.resize(shared);
        key_.append(p, unshared);
        value_ = std::string_view(p + unshared, value_len);
        while (restart_index_ + 1 < num_restarts_ && restart_point(restart_index_ + 1) < current_) {
            ++restart_index_;
        }
        return true;
    }

    void corrupted() {
        current_ = restarts_;
        restart_index_ = num_restarts_;
        status_ = Status::corruption("bad entry in block");
        key_.clear();
        value_ = {};
    }

    std::shared_ptr<const Block> block_;
    const Comparator* comparator_;
    const char* data_;
    std::uint32_t restarts_;  // offset of the restart array
    std::uint32_t num_restarts_;
    std::uint32_t current_;        // offset of the current entry; >= restarts_ if invalid
    std::uint32_t restart_index_;  // restart block containing current_
    std::string key_;
    std::string_view value_;
    Status status_;
};

std::unique_ptr<Iterator> Block::new_iterator(std::shared_ptr<const Block> block, const Comparator* comparator) {
    return std::make_unique<BlockIterator>(std::move(block), comparator);
}

}  // namespace strata
