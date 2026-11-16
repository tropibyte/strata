// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// SkipList: the ordered structure behind the memtable.
//
// Concurrency: one writer at a time (the caller serializes inserts), any
// number of concurrent readers without locks. A node is fully built before
// it is published with a release store, and readers follow links with
// acquire loads, so a reader sees either the old list or the new node with
// its key intact. Nodes are never removed; the list lives until its Arena
// is destroyed.
//
// Key must be trivially copyable (the memtable uses const char*), and Compare
// is a function object returning <0, 0 or >0.

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>

#include "util/arena.hpp"
#include "util/random.hpp"

namespace strata {

template <class Key, class Compare>
class SkipList {
    static_assert(std::is_trivially_copyable_v<Key>, "skiplist keys are copied without locks");

    struct Node;

public:
    SkipList(Compare compare, Arena* arena)
        : compare_(compare), arena_(arena), head_(new_node(Key{}, kMaxHeight)), rnd_(0x5eed5eedu) {
        for (int i = 0; i < kMaxHeight; ++i) head_->set_next(i, nullptr);
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    /// Requires that nothing equal to key is in the list, and external
    /// synchronization with other inserts.
    void insert(const Key& key) {
        Node* prev[kMaxHeight];
        Node* x = find_greater_or_equal(key, prev);
        assert(x == nullptr || !equal(key, x->key));
        (void)x;

        const int height = random_height();
        const int current = max_height_.load(std::memory_order_relaxed);
        if (height > current) {
            for (int i = current; i < height; ++i) prev[i] = head_;
            // Readers that see the new height before the node is linked just
            // find head_'s null links at those levels and move down: fine.
            max_height_.store(height, std::memory_order_relaxed);
        }

        x = new_node(key, height);
        for (int i = 0; i < height; ++i) {
            // The node's own links need no barrier: it is not reachable yet.
            x->set_next_relaxed(i, prev[i]->next_relaxed(i));
            prev[i]->set_next(i, x);
        }
    }

    bool contains(const Key& key) const {
        Node* x = find_greater_or_equal(key, nullptr);
        return x != nullptr && equal(key, x->key);
    }

    class Iterator {
    public:
        explicit Iterator(const SkipList* list) : list_(list) {}

        bool valid() const noexcept { return node_ != nullptr; }
        const Key& key() const {
            assert(valid());
            return node_->key;
        }
        void next() {
            assert(valid());
            node_ = node_->next(0);
        }
        void prev() {
            // No back links: search for the last node before this key.
            assert(valid());
            node_ = list_->find_less_than(node_->key);
            if (node_ == list_->head_) node_ = nullptr;
        }
        /// First entry >= target.
        void seek(const Key& target) { node_ = list_->find_greater_or_equal(target, nullptr); }
        void seek_to_first() { node_ = list_->head_->next(0); }
        void seek_to_last() {
            node_ = list_->find_last();
            if (node_ == list_->head_) node_ = nullptr;
        }

    private:
        const SkipList* list_;
        Node* node_ = nullptr;
    };

private:
    static constexpr int kMaxHeight = 12;
    static constexpr unsigned kBranching = 4;

    struct Node {
        explicit Node(const Key& k) : key(k) {}

        Key const key;

        Node* next(int n) { return links()[n].load(std::memory_order_acquire); }
        void set_next(int n, Node* x) { links()[n].store(x, std::memory_order_release); }
        Node* next_relaxed(int n) { return links()[n].load(std::memory_order_relaxed); }
        void set_next_relaxed(int n, Node* x) { links()[n].store(x, std::memory_order_relaxed); }

        // The links are laid out right after the node, one per level.
        std::atomic<Node*>* links() { return reinterpret_cast<std::atomic<Node*>*>(this + 1); }
    };

    static_assert(alignof(Node) >= alignof(std::atomic<Node*>) || sizeof(Node) % alignof(std::atomic<Node*>) == 0,
                  "links must be aligned after the node");

    Node* new_node(const Key& key, int height) {
        const std::size_t bytes = sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<std::size_t>(height);
        char* mem = arena_->allocate_aligned(bytes);
        Node* node = new (mem) Node(key);
        auto* links = reinterpret_cast<std::atomic<Node*>*>(mem + sizeof(Node));
        for (int i = 0; i < height; ++i) new (&links[i]) std::atomic<Node*>(nullptr);
        return node;
    }

    int random_height() {
        int height = 1;
        while (height < kMaxHeight && rnd_.one_in(kBranching)) ++height;
        return height;
    }

    bool equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

    bool key_is_after_node(const Key& key, Node* n) const { return n != nullptr && compare_(n->key, key) < 0; }

    // First node >= key; fills prev[level] with the last node < key at each
    // level when prev is non-null.
    Node* find_greater_or_equal(const Key& key, Node** prev) const {
        Node* x = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        for (;;) {
            Node* next = x->next(level);
            if (key_is_after_node(key, next)) {
                x = next;
            } else {
                if (prev != nullptr) prev[level] = x;
                if (level == 0) return next;
                --level;
            }
        }
    }

    // Last node < key, or head_.
    Node* find_less_than(const Key& key) const {
        Node* x = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        for (;;) {
            Node* next = x->next(level);
            if (next == nullptr || compare_(next->key, key) >= 0) {
                if (level == 0) return x;
                --level;
            } else {
                x = next;
            }
        }
    }

    // Last node, or head_ if empty.
    Node* find_last() const {
        Node* x = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        for (;;) {
            Node* next = x->next(level);
            if (next == nullptr) {
                if (level == 0) return x;
                --level;
            } else {
                x = next;
            }
        }
    }

    Compare const compare_;
    Arena* const arena_;
    Node* const head_;
    std::atomic<int> max_height_{1};
    Random rnd_;  // used only by the writer
};

}  // namespace strata
