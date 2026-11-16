// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/memtable.hpp"

#include <cstring>

#include "util/coding.hpp"

namespace strata {
namespace {

// Reads a length-prefixed slice from an entry the memtable wrote itself.
std::string_view get_length_prefixed_unchecked(const char* p) {
    std::uint32_t len = 0;
    const char* q = decode_varint32(p, p + kMaxVarint32Bytes, &len);
    return std::string_view(q, len);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
    return comparator->compare(get_length_prefixed_unchecked(a), get_length_prefixed_unchecked(b));
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), table_(KeyComparator{&comparator_}, &arena_) {}

void MemTable::add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value) {
    const std::size_t internal_key_size = key.size() + kInternalKeyTagSize;
    const std::size_t encoded_len = static_cast<std::size_t>(varint_length(internal_key_size)) + internal_key_size +
                                    static_cast<std::size_t>(varint_length(value.size())) + value.size();
    char* buf = arena_.allocate(encoded_len);
    char* p = encode_varint32(buf, static_cast<std::uint32_t>(internal_key_size));
    std::memcpy(p, key.data(), key.size());
    p += key.size();
    encode_fixed64(p, pack_sequence_and_type(seq, type));
    p += kInternalKeyTagSize;
    p = encode_varint32(p, static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) std::memcpy(p, value.data(), value.size());
    table_.insert(buf);
    entries_.fetch_add(1, std::memory_order_relaxed);
    if (type == ValueType::Deletion) deletions_.fetch_add(1, std::memory_order_relaxed);
}

bool MemTable::get(const LookupKey& key, std::string* value, Status* status) const {
    const std::string_view memkey = key.memtable_key();
    Table::Iterator iter(&table_);
    iter.seek(memkey.data());
    if (!iter.valid()) return false;

    // The seek landed on the first entry >= (user_key, sequence). It belongs
    // to our key only if the user keys match.
    const char* entry = iter.key();
    std::uint32_t key_length = 0;
    const char* key_ptr = decode_varint32(entry, entry + kMaxVarint32Bytes, &key_length);
    const std::string_view internal_key(key_ptr, key_length);
    if (comparator_.user_comparator()->compare(extract_user_key(internal_key), key.user_key()) != 0) return false;

    const std::uint64_t tag = decode_fixed64(key_ptr + key_length - kInternalKeyTagSize);
    switch (static_cast<ValueType>(tag & 0xffu)) {
        case ValueType::Value: {
            const std::string_view v = get_length_prefixed_unchecked(key_ptr + key_length);
            value->assign(v.data(), v.size());
            *status = Status::ok();
            return true;
        }
        case ValueType::Deletion:
            *status = Status::not_found("deleted");
            return true;
    }
    return false;
}

class MemTableIterator final : public Iterator {
public:
    explicit MemTableIterator(std::shared_ptr<const MemTable> mem) : mem_(std::move(mem)), iter_(&mem_->table_) {}

    bool valid() const override { return iter_.valid(); }

    void seek(std::string_view target) override {
        // The skiplist holds length-prefixed keys; encode the target the same way.
        scratch_.clear();
        put_varint32(scratch_, static_cast<std::uint32_t>(target.size()));
        scratch_.append(target);
        iter_.seek(scratch_.data());
    }

    void seek_to_first() override { iter_.seek_to_first(); }
    void seek_to_last() override { iter_.seek_to_last(); }
    void next() override { iter_.next(); }
    void prev() override { iter_.prev(); }

    std::string_view key() const override { return get_length_prefixed_unchecked(iter_.key()); }

    std::string_view value() const override {
        const std::string_view k = get_length_prefixed_unchecked(iter_.key());
        return get_length_prefixed_unchecked(k.data() + k.size());
    }

    Status status() const override { return Status::ok(); }

private:
    std::shared_ptr<const MemTable> mem_;
    MemTable::Table::Iterator iter_;
    std::string scratch_;
};

std::unique_ptr<Iterator> MemTable::new_iterator() const {
    return std::make_unique<MemTableIterator>(shared_from_this());
}

}  // namespace strata
