// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/db_iter.hpp"

#include <cassert>
#include <string>

namespace strata {
namespace {

// Internal entries for one user key are ordered newest first. Moving
// forward, the iterator stops on the newest visible entry of each key.
// Moving backward it sees the entries of a key oldest first, so it reads all
// of them and remembers the last (newest) one in saved_key_/saved_value_.
//
// Invariants:
//   Forward:  iter_ is at the entry that supplies key() and value().
//   Reverse:  iter_ is just before every entry of key() (or invalid), and
//             key()/value() come from saved_key_/saved_value_.
class DBIter final : public Iterator {
public:
    DBIter(const Comparator* ucmp, std::unique_ptr<Iterator> iter, SequenceNumber sequence,
           std::vector<std::shared_ptr<const void>> pins)
        : ucmp_(ucmp), pins_(std::move(pins)), iter_(std::move(iter)), sequence_(sequence) {}

    bool valid() const override { return valid_; }

    std::string_view key() const override {
        assert(valid_);
        return direction_ == Direction::Forward ? extract_user_key(iter_->key()) : std::string_view(saved_key_);
    }

    std::string_view value() const override {
        assert(valid_);
        return direction_ == Direction::Forward ? iter_->value() : std::string_view(saved_value_);
    }

    Status status() const override {
        if (!status_.is_ok()) return status_;
        return iter_->status();
    }

    void next() override {
        assert(valid_);
        if (direction_ == Direction::Reverse) {
            direction_ = Direction::Forward;
            // iter_ is before the entries of key(); step into them. saved_key_
            // already holds key(), the key to skip.
            if (!iter_->valid()) {
                iter_->seek_to_first();
            } else {
                iter_->next();
            }
            if (!iter_->valid()) {
                invalidate();
                return;
            }
        } else {
            saved_key_.assign(extract_user_key(iter_->key()));
            iter_->next();
            if (!iter_->valid()) {
                invalidate();
                return;
            }
        }
        find_next_user_entry(/*skipping=*/true);
    }

    void prev() override {
        assert(valid_);
        if (direction_ == Direction::Forward) {
            // iter_ is at the current entry. Back up past every entry of the
            // current key, so iter_ is just before them.
            saved_key_.assign(extract_user_key(iter_->key()));
            for (;;) {
                iter_->prev();
                if (!iter_->valid()) {
                    invalidate();
                    return;
                }
                if (ucmp_->compare(extract_user_key(iter_->key()), saved_key_) < 0) break;
            }
            direction_ = Direction::Reverse;
        }
        find_prev_user_entry();
    }

    void seek(std::string_view target) override {
        direction_ = Direction::Forward;
        saved_value_.clear();
        saved_key_.clear();
        append_internal_key(saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
        iter_->seek(saved_key_);
        if (iter_->valid()) {
            find_next_user_entry(/*skipping=*/false);
        } else {
            invalidate();
        }
    }

    void seek_to_first() override {
        direction_ = Direction::Forward;
        saved_value_.clear();
        iter_->seek_to_first();
        if (iter_->valid()) {
            find_next_user_entry(/*skipping=*/false);
        } else {
            invalidate();
        }
    }

    void seek_to_last() override {
        direction_ = Direction::Reverse;
        saved_value_.clear();
        iter_->seek_to_last();
        find_prev_user_entry();
    }

private:
    enum class Direction { Forward, Reverse };

    bool parse(ParsedInternalKey* ikey) {
        if (parse_internal_key(iter_->key(), ikey)) return true;
        if (status_.is_ok()) status_ = Status::corruption("corrupted internal key in DBIter");
        return false;
    }

    void invalidate() {
        valid_ = false;
        saved_key_.clear();
        saved_value_.clear();
    }

    // Moves forward from iter_ (inclusive) to the newest visible entry of a
    // user key that is not deleted. With `skipping`, entries for user keys
    // <= saved_key_ are hidden (they are the key we are leaving, or keys
    // covered by a deletion).
    void find_next_user_entry(bool skipping) {
        assert(iter_->valid());
        assert(direction_ == Direction::Forward);
        do {
            ParsedInternalKey ikey;
            if (parse(&ikey) && ikey.sequence <= sequence_) {
                switch (ikey.type) {
                    case ValueType::Deletion:
                        // Hide every older entry of this key.
                        saved_key_.assign(ikey.user_key);
                        skipping = true;
                        break;
                    case ValueType::Value:
                        if (skipping && ucmp_->compare(ikey.user_key, saved_key_) <= 0) break;  // hidden
                        valid_ = true;
                        saved_key_.clear();
                        return;
                }
            }
            iter_->next();
        } while (iter_->valid());
        invalidate();
    }

    // Moves backward from iter_ (inclusive) and leaves the previous visible
    // user key in saved_key_/saved_value_, with iter_ just before its entries.
    void find_prev_user_entry() {
        assert(direction_ == Direction::Reverse);
        ValueType value_type = ValueType::Deletion;
        while (iter_->valid()) {
            ParsedInternalKey ikey;
            if (parse(&ikey) && ikey.sequence <= sequence_) {
                if (value_type != ValueType::Deletion && ucmp_->compare(ikey.user_key, saved_key_) < 0) {
                    break;  // reached the key before the one we found
                }
                value_type = ikey.type;
                if (value_type == ValueType::Deletion) {
                    saved_key_.clear();
                    saved_value_.clear();
                } else {
                    saved_key_.assign(ikey.user_key);
                    saved_value_.assign(iter_->value());
                }
            }
            iter_->prev();
        }

        if (value_type == ValueType::Deletion) {
            // Ran off the beginning without finding a live key.
            invalidate();
            direction_ = Direction::Forward;
        } else {
            valid_ = true;
        }
    }

    const Comparator* ucmp_;
    // Declared before iter_ so they are destroyed after it.
    std::vector<std::shared_ptr<const void>> pins_;
    std::unique_ptr<Iterator> iter_;
    SequenceNumber sequence_;

    Status status_;
    Direction direction_ = Direction::Forward;
    bool valid_ = false;
    std::string saved_key_;    // see the class comment
    std::string saved_value_;  // value of the current entry in Reverse
};

}  // namespace

std::unique_ptr<Iterator> new_db_iterator(const Comparator* user_comparator, std::unique_ptr<Iterator> internal,
                                          SequenceNumber sequence, std::vector<std::shared_ptr<const void>> pins) {
    return std::make_unique<DBIter>(user_comparator, std::move(internal), sequence, std::move(pins));
}

}  // namespace strata
