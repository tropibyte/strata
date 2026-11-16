// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "table/format.hpp"

#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/lz.hpp"

namespace strata {

void BlockHandle::encode_to(std::string& dst) const {
    put_varint64(dst, offset_);
    put_varint64(dst, size_);
}

Status BlockHandle::decode_from(std::string_view& input) {
    if (get_varint64(input, &offset_) && get_varint64(input, &size_)) return Status::ok();
    return Status::corruption("bad block handle");
}

void Footer::encode_to(std::string& dst) const {
    const std::size_t start = dst.size();
    metaindex.encode_to(dst);
    index.encode_to(dst);
    dst.resize(start + kHandlesLength, '\0');
    put_fixed64(dst, kTableMagic);
}

Status Footer::decode_from(std::string_view input) {
    if (input.size() != kEncodedLength) return Status::corruption("footer has the wrong size");
    const std::uint64_t magic = decode_fixed64(input.data() + kHandlesLength);
    if (magic != kTableMagic) return Status::corruption("not a strata table (bad magic number)");
    std::string_view handles = input.substr(0, kHandlesLength);
    Status s = metaindex.decode_from(handles);
    if (s.is_ok()) s = index.decode_from(handles);
    return s;
}

Status read_block(const RandomAccessFile& file, const BlockHandle& handle, bool verify, std::string* contents) {
    // Refuse absurd sizes from a damaged handle before allocating.
    if (handle.size() > (std::uint64_t{1} << 30)) return Status::corruption("block handle size is too large");
    const auto n = static_cast<std::size_t>(handle.size());
    std::string buf;
    Status s = file.read(handle.offset(), n + kBlockTrailerSize, buf);
    if (!s.is_ok()) return s;
    if (buf.size() != n + kBlockTrailerSize) return Status::corruption("truncated block read");

    const auto type = static_cast<std::uint8_t>(buf[n]);
    if (verify || type != static_cast<std::uint8_t>(Compression::None)) {
        const std::uint32_t expected = crc32c::unmask(decode_fixed32(buf.data() + n + 1));
        const std::uint32_t actual = crc32c::value(buf.data(), n + 1);
        if (actual != expected) return Status::corruption("block checksum mismatch");
    }

    switch (type) {
        case static_cast<std::uint8_t>(Compression::None):
            buf.resize(n);
            *contents = std::move(buf);
            return Status::ok();
        case static_cast<std::uint8_t>(Compression::Lz):
            if (!lz::decompress(std::string_view(buf.data(), n), *contents)) {
                return Status::corruption("corrupted compressed block");
            }
            return Status::ok();
        default:
            return Status::corruption("unknown block compression type");
    }
}

Compression compress_block(std::string_view raw, Compression wanted, std::string* out) {
    out->clear();
    if (wanted == Compression::Lz) {
        lz::compress(raw, *out);
        if (out->size() < raw.size() - raw.size() / 8) return Compression::Lz;
        out->clear();
    }
    out->assign(raw);
    return Compression::None;
}

}  // namespace strata
