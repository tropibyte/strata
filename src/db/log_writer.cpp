// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/log_writer.hpp"

#include <algorithm>
#include <cassert>

#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::log {

Writer::Writer(WritableFile* dest, std::uint64_t dest_length)
    : dest_(dest), block_offset_(static_cast<std::size_t>(dest_length % kBlockSize)) {
    for (int i = 0; i <= kMaxRecordType; ++i) {
        const char t = static_cast<char>(i);
        type_crc_[static_cast<std::size_t>(i)] = crc32c::value(&t, 1);
    }
}

Status Writer::add_record(std::string_view record) {
    const char* ptr = record.data();
    std::size_t left = record.size();

    // Emit at least one fragment, so an empty record is still a record.
    Status s;
    bool begin = true;
    do {
        const std::size_t leftover = kBlockSize - block_offset_;
        if (leftover < kHeaderSize) {
            // Too little room for a header: zero-fill and start a new block.
            if (leftover > 0) {
                static constexpr char kZeros[kHeaderSize] = {};
                s = dest_->append(std::string_view(kZeros, leftover));
                if (!s.is_ok()) return s;
            }
            block_offset_ = 0;
        }

        const std::size_t avail = kBlockSize - block_offset_ - kHeaderSize;
        const std::size_t fragment = std::min(left, avail);
        const bool end = (left == fragment);
        RecordType type;
        if (begin && end) {
            type = kFullType;
        } else if (begin) {
            type = kFirstType;
        } else if (end) {
            type = kLastType;
        } else {
            type = kMiddleType;
        }

        s = emit_fragment(type, ptr, fragment);
        ptr += fragment;
        left -= fragment;
        begin = false;
    } while (s.is_ok() && left > 0);
    return s;
}

Status Writer::emit_fragment(RecordType type, const char* data, std::size_t length) {
    assert(length <= 0xffff);
    assert(block_offset_ + kHeaderSize + length <= kBlockSize);

    char header[kHeaderSize];
    header[4] = static_cast<char>(length & 0xffu);
    header[5] = static_cast<char>(length >> 8);
    header[6] = static_cast<char>(type);
    std::uint32_t crc = crc32c::extend(type_crc_[type], data, length);
    encode_fixed32(header, crc32c::mask(crc));

    Status s = dest_->append(std::string_view(header, kHeaderSize));
    if (s.is_ok()) {
        s = dest_->append(std::string_view(data, length));
        if (s.is_ok()) s = dest_->flush();
    }
    block_offset_ += kHeaderSize + length;
    return s;
}

}  // namespace strata::log
