// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// LruCache: a sharded, thread-safe least-recently-used cache with a byte
// budget. Values are held by shared_ptr, so an entry evicted while a reader
// still uses it stays alive until the reader lets go; the cache stops
// counting it against the budget as soon as it is evicted.
//
// Used for data blocks (keyed by table id and block offset) and for open
// table files (keyed by file number).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace strata {

template <class Key, class Value, class Hash = std::hash<Key>>
class LruCache {
public:
    /// capacity: total charge allowed. 0 disables caching (insert is a no-op).
    /// shard_bits: the cache is split into 2^shard_bits independently locked
    /// shards to reduce contention.
    explicit LruCache(std::size_t capacity, int shard_bits = 4)
        : capacity_(capacity), shards_(std::size_t{1} << shard_bits) {
        const std::size_t n = shards_.size();
        for (auto& shard : shards_) shard.capacity = (capacity + n - 1) / n;
    }

    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    /// The cached value, or null. A hit makes the entry most recently used.
    std::shared_ptr<Value> lookup(const Key& key) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.index.find(key);
        if (it == shard.index.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second->value;
    }

    /// Inserts or replaces. Evicts least recently used entries until the
    /// shard is within budget (possibly evicting this one, if it is larger
    /// than a whole shard).
    void insert(const Key& key, std::shared_ptr<Value> value, std::size_t charge) {
        if (capacity_ == 0) return;
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.index.find(key);
        if (it != shard.index.end()) {
            shard.usage -= it->second->charge;
            shard.lru.erase(it->second);
            shard.index.erase(it);
        }
        shard.lru.push_front(Entry{key, std::move(value), charge});
        shard.index.emplace(key, shard.lru.begin());
        shard.usage += charge;
        while (shard.usage > shard.capacity && !shard.lru.empty()) {
            Entry& victim = shard.lru.back();
            shard.usage -= victim.charge;
            shard.index.erase(victim.key);
            shard.lru.pop_back();
        }
    }

    void erase(const Key& key) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.index.find(key);
        if (it == shard.index.end()) return;
        shard.usage -= it->second->charge;
        shard.lru.erase(it->second);
        shard.index.erase(it);
    }

    /// Removes every entry.
    void clear() {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.lru.clear();
            shard.index.clear();
            shard.usage = 0;
        }
    }

    std::size_t total_charge() const {
        std::size_t total = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            total += shard.usage;
        }
        return total;
    }

    std::size_t size() const {
        std::size_t total = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            total += shard.lru.size();
        }
        return total;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::uint64_t hits() const noexcept { return hits_.load(std::memory_order_relaxed); }
    std::uint64_t misses() const noexcept { return misses_.load(std::memory_order_relaxed); }

    /// A fresh id; table readers use one each to build distinct block keys.
    std::uint64_t new_id() noexcept { return next_id_.fetch_add(1, std::memory_order_relaxed) + 1; }

private:
    struct Entry {
        Key key;
        std::shared_ptr<Value> value;
        std::size_t charge;
    };

    struct Shard {
        mutable std::mutex mutex;
        std::list<Entry> lru;  // front = most recently used
        std::unordered_map<Key, typename std::list<Entry>::iterator, Hash> index;
        std::size_t usage = 0;
        std::size_t capacity = 0;
    };

    Shard& shard_for(const Key& key) {
        // Mix the hash so that shard choice uses its high-quality bits.
        std::uint64_t h = static_cast<std::uint64_t>(Hash{}(key));
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        return shards_[static_cast<std::size_t>(h) & (shards_.size() - 1)];
    }

    std::size_t capacity_;
    std::vector<Shard> shards_;
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace strata
