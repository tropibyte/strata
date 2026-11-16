// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/merging_iterator.hpp"

#include "strata/comparator.hpp"
#include "table/iterator_wrapper.hpp"

namespace strata {
namespace {

// Finds the smallest (or largest) child by scanning all of them. The number
// of children is small — one per memtable, level-0 file and deeper level —
// so a linear scan is cheap and keeps direction changes simple.
class MergingIterator final : public Iterator {
public:
    MergingIterator(const Comparator* comparator, std::vector<std::unique_ptr<Iterator>> children)
        : comparator_(comparator), children_(children.size()) {
        for (std::size_t i = 0; i < children.size(); ++i) children_[i].set(std::move(children[i]));
    }

    bool valid() const override { return current_ != nullptr; }

    void seek_to_first() override {
        for (auto& child : children_) child.seek_to_first();
        find_smallest();
        direction_ = Direction::Forward;
    }

    void seek_to_last() override {
        for (auto& child : children_) child.seek_to_last();
        find_largest();
        direction_ = Direction::Reverse;
    }

    void seek(std::string_view target) override {
        for (auto& child : children_) child.seek(target);
        find_smallest();
        direction_ = Direction::Forward;
    }

    void next() override {
        // Every non-current child must be positioned after key(). Going
        // forward that already holds; after a reverse move the other
        // children sit before key(), so move each to its first entry > key().
        if (direction_ != Direction::Forward) {
            const std::string saved(key());
            for (auto& child : children_) {
                if (&child == current_) continue;
                child.seek(saved);
                if (child.valid() && comparator_->compare(saved, child.key()) == 0) child.next();
            }
            direction_ = Direction::Forward;
        }
        current_->next();
        find_smallest();
    }

    void prev() override {
        // Mirror of next(): put every other child at its last entry < key().
        if (direction_ != Direction::Reverse) {
            const std::string saved(key());
            for (auto& child : children_) {
                if (&child == current_) continue;
                child.seek(saved);
                if (child.valid()) {
                    child.prev();  // now at the last entry < saved
                } else {
                    child.seek_to_last();  // every entry is < saved
                }
            }
            direction_ = Direction::Reverse;
        }
        current_->prev();
        find_largest();
    }

    std::string_view key() const override { return current_->key(); }
    std::string_view value() const override { return current_->value(); }

    Status status() const override {
        for (const auto& child : children_) {
            Status s = child.status();
            if (!s.is_ok()) return s;
        }
        return Status::ok();
    }

private:
    enum class Direction { Forward, Reverse };

    void find_smallest() {
        IteratorWrapper* smallest = nullptr;
        for (auto& child : children_) {
            if (!child.valid()) continue;
            if (smallest == nullptr || comparator_->compare(child.key(), smallest->key()) < 0) smallest = &child;
        }
        current_ = smallest;
    }

    void find_largest() {
        IteratorWrapper* largest = nullptr;
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            if (!it->valid()) continue;
            if (largest == nullptr || comparator_->compare(it->key(), largest->key()) > 0) largest = &*it;
        }
        current_ = largest;
    }

    const Comparator* comparator_;
    std::vector<IteratorWrapper> children_;
    IteratorWrapper* current_ = nullptr;
    Direction direction_ = Direction::Forward;
};

}  // namespace

std::unique_ptr<Iterator> new_merging_iterator(const Comparator* comparator,
                                               std::vector<std::unique_ptr<Iterator>> children) {
    if (children.empty()) return new_empty_iterator();
    if (children.size() == 1) return std::move(children[0]);
    return std::make_unique<MergingIterator>(comparator, std::move(children));
}

}  // namespace strata
