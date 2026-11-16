// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/log_reader.hpp"

#include "util/coding.hpp"
#include "util/crc32c.hpp"

namespace strata::log {

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum)
    : file_(file), reporter_(reporter), checksum_(checksum) {}

void Reader::report_drop(std::size_t bytes, std::string_view reason) {
    if (reporter_ != nullptr) reporter_->corruption(bytes, Status::corruption(reason));
}

bool Reader::read_record(std::string* record) {
    record->clear();
    bool in_fragmented_record = false;
    std::uint64_t prospective_offset = 0;  // offset of the record being assembled

    for (;;) {
        std::string_view fragment;
        const unsigned type = read_physical_record(&fragment);
        // Offset of the fragment just read.
        const std::uint64_t fragment_offset = buffer_end_offset_ - buffer_.size() - kHeaderSize - fragment.size();

        switch (type) {
            case kFullType:
                if (in_fragmented_record && !record->empty()) {
                    report_drop(record->size(), "partial record without end");
                }
                record->assign(fragment);
                last_record_offset_ = fragment_offset;
                return true;

            case kFirstType:
                if (in_fragmented_record && !record->empty()) {
                    report_drop(record->size(), "partial record without end");
                }
                prospective_offset = fragment_offset;
                record->assign(fragment);
                in_fragmented_record = true;
                break;

            case kMiddleType:
                if (!in_fragmented_record) {
                    report_drop(fragment.size(), "missing start of fragmented record");
                } else {
                    record->append(fragment);
                }
                break;

            case kLastType:
                if (!in_fragmented_record) {
                    report_drop(fragment.size(), "missing start of fragmented record");
                } else {
                    record->append(fragment);
                    last_record_offset_ = prospective_offset;
                    return true;
                }
                break;

            case kEof:
                // A record cut short at the end is a write that never
                // finished; drop it without complaint.
                record->clear();
                return false;

            case kBadRecord:
                if (in_fragmented_record) {
                    report_drop(record->size(), "error in middle of record");
                    in_fragmented_record = false;
                    record->clear();
                }
                break;

            default:
                report_drop(fragment.size() + (in_fragmented_record ? record->size() : 0), "unknown record type");
                in_fragmented_record = false;
                record->clear();
                break;
        }
    }
}

unsigned Reader::read_physical_record(std::string_view* fragment) {
    for (;;) {
        if (buffer_.size() < kHeaderSize) {
            if (eof_) {
                // A truncated header at the very end: the writer died while
                // writing it.
                buffer_ = {};
                return kEof;
            }
            // Skip the zero-filled trailer of this block and read the next.
            Status s = file_->read(kBlockSize, backing_);
            buffer_ = backing_;
            buffer_end_offset_ += backing_.size();
            if (!s.is_ok()) {
                buffer_ = {};
                report_drop(kBlockSize, s.message());
                eof_ = true;
                return kEof;
            }
            if (backing_.size() < kBlockSize) eof_ = true;
            continue;
        }

        const char* header = buffer_.data();
        const std::size_t length = static_cast<std::size_t>(static_cast<std::uint8_t>(header[4])) |
                                   (static_cast<std::size_t>(static_cast<std::uint8_t>(header[5])) << 8);
        const auto type = static_cast<std::uint8_t>(header[6]);

        if (kHeaderSize + length > buffer_.size()) {
            const std::size_t drop = buffer_.size();
            buffer_ = {};
            if (!eof_) {
                report_drop(drop, "bad record length");
                return kBadRecord;
            }
            // The payload was cut short by the end of the file: a torn write.
            return kEof;
        }

        if (type == kZeroType && length == 0) {
            // Zero-filled space (for example a preallocated file). Skip the
            // rest of the block silently.
            buffer_ = {};
            return kBadRecord;
        }

        if (checksum_) {
            const std::uint32_t expected = crc32c::unmask(decode_fixed32(header));
            const std::uint32_t actual = crc32c::value(header + 6, 1 + length);
            if (actual != expected) {
                // The length itself may be damaged; drop the rest of the
                // block rather than trust it.
                const std::size_t drop = buffer_.size();
                buffer_ = {};
                report_drop(drop, "checksum mismatch");
                return kBadRecord;
            }
        }

        *fragment = std::string_view(header + kHeaderSize, length);
        buffer_.remove_prefix(kHeaderSize + length);
        return type;
    }
}

}  // namespace strata::log
