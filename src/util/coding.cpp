// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/coding.hpp"

namespace strata {

void encode_fixed32(char* dst, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) {
        dst[i] = static_cast<char>(static_cast<std::uint8_t>(value & 0xffu));
        value >>= 8;
    }
}

void encode_fixed64(char* dst, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<char>(static_cast<std::uint8_t>(value & 0xffu));
        value >>= 8;
    }
}

std::uint32_t decode_fixed32(const char* p) noexcept {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint8_t>(p[i]);
    }
    return value;
}

std::uint64_t decode_fixed64(const char* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint8_t>(p[i]);
    }
    return value;
}

void put_fixed32(std::string& dst, std::uint32_t value) {
    char buf[4];
    encode_fixed32(buf, value);
    dst.append(buf, sizeof(buf));
}

void put_fixed64(std::string& dst, std::uint64_t value) {
    char buf[8];
    encode_fixed64(buf, value);
    dst.append(buf, sizeof(buf));
}

char* encode_varint64(char* dst, std::uint64_t value) noexcept {
    while (value >= 0x80) {
        *dst++ = static_cast<char>(static_cast<std::uint8_t>((value & 0x7fu) | 0x80u));
        value >>= 7;
    }
    *dst++ = static_cast<char>(static_cast<std::uint8_t>(value));
    return dst;
}

char* encode_varint32(char* dst, std::uint32_t value) noexcept {
    return encode_varint64(dst, value);
}

void put_varint32(std::string& dst, std::uint32_t value) {
    char buf[kMaxVarint32Bytes];
    char* end = encode_varint32(buf, value);
    dst.append(buf, static_cast<std::size_t>(end - buf));
}

void put_varint64(std::string& dst, std::uint64_t value) {
    char buf[kMaxVarint64Bytes];
    char* end = encode_varint64(buf, value);
    dst.append(buf, static_cast<std::size_t>(end - buf));
}

void put_length_prefixed(std::string& dst, std::string_view value) {
    put_varint32(dst, static_cast<std::uint32_t>(value.size()));
    dst.append(value);
}

int varint_length(std::uint64_t value) noexcept {
    int len = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++len;
    }
    return len;
}

const char* decode_varint64(const char* p, const char* limit, std::uint64_t* value) noexcept {
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift <= 63 && p < limit; shift += 7) {
        const auto byte = static_cast<std::uint8_t>(*p++);
        if (shift == 63 && byte > 1) return nullptr;  // overflows 64 bits
        if (byte & 0x80u) {
            result |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
        } else {
            result |= static_cast<std::uint64_t>(byte) << shift;
            *value = result;
            return p;
        }
    }
    return nullptr;
}

const char* decode_varint32(const char* p, const char* limit, std::uint32_t* value) noexcept {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift <= 28 && p < limit; shift += 7) {
        const auto byte = static_cast<std::uint8_t>(*p++);
        if (shift == 28 && byte > 0x0f) return nullptr;  // overflows 32 bits
        if (byte & 0x80u) {
            result |= static_cast<std::uint32_t>(byte & 0x7fu) << shift;
        } else {
            result |= static_cast<std::uint32_t>(byte) << shift;
            *value = result;
            return p;
        }
    }
    return nullptr;
}

bool get_fixed32(std::string_view& input, std::uint32_t* value) {
    if (input.size() < 4) return false;
    *value = decode_fixed32(input.data());
    input.remove_prefix(4);
    return true;
}

bool get_fixed64(std::string_view& input, std::uint64_t* value) {
    if (input.size() < 8) return false;
    *value = decode_fixed64(input.data());
    input.remove_prefix(8);
    return true;
}

bool get_varint32(std::string_view& input, std::uint32_t* value) {
    const char* p = input.data();
    const char* limit = p + input.size();
    const char* q = decode_varint32(p, limit, value);
    if (q == nullptr) return false;
    input.remove_prefix(static_cast<std::size_t>(q - p));
    return true;
}

bool get_varint64(std::string_view& input, std::uint64_t* value) {
    const char* p = input.data();
    const char* limit = p + input.size();
    const char* q = decode_varint64(p, limit, value);
    if (q == nullptr) return false;
    input.remove_prefix(static_cast<std::size_t>(q - p));
    return true;
}

bool get_length_prefixed(std::string_view& input, std::string_view* value) {
    std::uint32_t len = 0;
    if (!get_varint32(input, &len) || input.size() < len) return false;
    *value = input.substr(0, len);
    input.remove_prefix(len);
    return true;
}

}  // namespace strata
