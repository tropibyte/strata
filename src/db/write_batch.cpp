// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "strata/write_batch.hpp"

#include <cassert>

#include "db/memtable.hpp"
#include "db/write_batch_internal.hpp"
#include "util/coding.hpp"

namespace strata {

namespace {
constexpr std::size_t kHeader = WriteBatchInternal::kHeaderSize;
}

WriteBatch::WriteBatch() {
    clear();
}

void WriteBatch::clear() {
    rep_.clear();
    rep_.resize(kHeader);
}

std::uint32_t WriteBatch::count() const {
    return WriteBatchInternal::count(*this);
}

void WriteBatch::put(std::string_view key, std::string_view value) {
    WriteBatchInternal::set_count(*this, count() + 1);
    rep_.push_back(static_cast<char>(ValueType::Value));
    put_length_prefixed(rep_, key);
    put_length_prefixed(rep_, value);
}

void WriteBatch::remove(std::string_view key) {
    WriteBatchInternal::set_count(*this, count() + 1);
    rep_.push_back(static_cast<char>(ValueType::Deletion));
    put_length_prefixed(rep_, key);
}

void WriteBatch::append(const WriteBatch& other) {
    WriteBatchInternal::append(*this, other);
}

Status WriteBatch::iterate(Handler& handler) const {
    std::string_view input(rep_);
    if (input.size() < kHeader) return Status::corruption("write batch is too small");
    input.remove_prefix(kHeader);

    std::uint32_t found = 0;
    while (!input.empty()) {
        ++found;
        const auto tag = static_cast<std::uint8_t>(input.front());
        input.remove_prefix(1);
        std::string_view key, value;
        switch (tag) {
            case static_cast<std::uint8_t>(ValueType::Value):
                if (!get_length_prefixed(input, &key) || !get_length_prefixed(input, &value)) {
                    return Status::corruption("bad write batch put");
                }
                handler.put(key, value);
                break;
            case static_cast<std::uint8_t>(ValueType::Deletion):
                if (!get_length_prefixed(input, &key)) return Status::corruption("bad write batch delete");
                handler.remove(key);
                break;
            default:
                return Status::corruption("unknown write batch tag");
        }
    }
    if (found != count()) return Status::corruption("write batch has the wrong count");
    return Status::ok();
}

std::uint32_t WriteBatchInternal::count(const WriteBatch& batch) {
    return decode_fixed32(batch.rep_.data() + 8);
}

void WriteBatchInternal::set_count(WriteBatch& batch, std::uint32_t n) {
    encode_fixed32(batch.rep_.data() + 8, n);
}

SequenceNumber WriteBatchInternal::sequence(const WriteBatch& batch) {
    return decode_fixed64(batch.rep_.data());
}

void WriteBatchInternal::set_sequence(WriteBatch& batch, SequenceNumber seq) {
    encode_fixed64(batch.rep_.data(), seq);
}

void WriteBatchInternal::set_contents(WriteBatch& batch, std::string_view contents) {
    assert(contents.size() >= kHeaderSize);
    batch.rep_.assign(contents);
}

void WriteBatchInternal::append(WriteBatch& dst, const WriteBatch& src) {
    set_count(dst, count(dst) + count(src));
    assert(src.rep_.size() >= kHeaderSize);
    dst.rep_.append(src.rep_, kHeaderSize, std::string::npos);
}

namespace {

class MemTableInserter final : public WriteBatch::Handler {
public:
    MemTableInserter(SequenceNumber seq, MemTable* mem) : sequence_(seq), mem_(mem) {}

    void put(std::string_view key, std::string_view value) override {
        mem_->add(sequence_, ValueType::Value, key, value);
        ++sequence_;
    }
    void remove(std::string_view key) override {
        mem_->add(sequence_, ValueType::Deletion, key, std::string_view());
        ++sequence_;
    }

private:
    SequenceNumber sequence_;
    MemTable* mem_;
};

}  // namespace

Status WriteBatchInternal::insert_into(const WriteBatch& batch, MemTable* mem) {
    MemTableInserter inserter(sequence(batch), mem);
    return batch.iterate(inserter);
}

}  // namespace strata
