// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/strutil.hpp"

#include <cstdio>
#include <limits>

namespace strata {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string escape_string(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto b = static_cast<unsigned char>(c);
        if (c == '\\') {
            out.append("\\\\");
        } else if (b >= 0x20 && b < 0x7f) {
            out.push_back(c);
        } else {
            out.append("\\x");
            out.push_back(kHexDigits[b >> 4]);
            out.push_back(kHexDigits[b & 0x0f]);
        }
    }
    return out;
}

bool unescape_string(std::string_view s, std::string* out) {
    out->clear();
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            out->push_back(s[i]);
            continue;
        }
        if (++i >= s.size()) return false;
        switch (s[i]) {
            case '\\':
                out->push_back('\\');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case '0':
                out->push_back('\0');
                break;
            case 'x': {
                if (i + 2 >= s.size()) return false;
                const int hi = hex_value(s[i + 1]);
                const int lo = hex_value(s[i + 2]);
                if (hi < 0 || lo < 0) return false;
                out->push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

std::string to_hex(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(kHexDigits[b >> 4]);
        out.push_back(kHexDigits[b & 0x0f]);
    }
    return out;
}

bool from_hex(std::string_view s, std::string* out) {
    if (s.size() % 2 != 0) return false;
    out->clear();
    out->reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int hi = hex_value(s[i]);
        const int lo = hex_value(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

bool consume_decimal(std::string_view& in, std::uint64_t* value) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t v = 0;
    std::size_t i = 0;
    for (; i < in.size() && in[i] >= '0' && in[i] <= '9'; ++i) {
        const auto digit = static_cast<std::uint64_t>(in[i] - '0');
        if (v > (kMax - digit) / 10) return false;
        v = v * 10 + digit;
    }
    if (i == 0) return false;
    in.remove_prefix(i);
    *value = v;
    return true;
}

std::string human_bytes(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[unit]);
    return buf;
}

std::string with_commas(std::uint64_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    const std::size_t len = digits.size();
    for (std::size_t i = 0; i < len; ++i) {
        if (i != 0 && (len - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace strata
