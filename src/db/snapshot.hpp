// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The list of live snapshots. Compaction keeps every entry a live snapshot
// can still see, so it needs the oldest one. Requires the DB mutex.

#pragma once

#include <cassert>
#include <list>

#include "db/dbformat.hpp"
#include "strata/db.hpp"

namespace strata {

class SnapshotImpl final : public Snapshot {
public:
    explicit SnapshotImpl(SequenceNumber seq) : sequence_(seq) {}
    ~SnapshotImpl() override = default;

    SequenceNumber sequence() const noexcept override { return sequence_; }

private:
    friend class SnapshotList;
    SequenceNumber sequence_;
    std::list<SnapshotImpl>::iterator self_;
};

class SnapshotList {
public:
    bool empty() const noexcept { return list_.empty(); }
    std::size_t size() const noexcept { return list_.size(); }

    SequenceNumber oldest() const {
        assert(!empty());
        return list_.front().sequence_;
    }
    SequenceNumber newest() const {
        assert(!empty());
        return list_.back().sequence_;
    }

    /// Sequence numbers must not decrease from one call to the next.
    const SnapshotImpl* create(SequenceNumber seq) {
        assert(empty() || newest() <= seq);
        list_.emplace_back(seq);
        auto it = std::prev(list_.end());
        it->self_ = it;
        return &*it;
    }

    void release(const SnapshotImpl* snapshot) { list_.erase(snapshot->self_); }

private:
    std::list<SnapshotImpl> list_;  // oldest first
};

}  // namespace strata
