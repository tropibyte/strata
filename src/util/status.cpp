// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "strata/status.hpp"

namespace strata {

Status::Status(Code code, std::string_view msg, std::string_view detail) : code_(code), msg_(msg) {
    if (!detail.empty()) {
        msg_.append(": ");
        msg_.append(detail);
    }
}

std::string_view code_name(Status::Code code) noexcept {
    switch (code) {
        case Status::Code::Ok:
            return "OK";
        case Status::Code::NotFound:
            return "NotFound";
        case Status::Code::Corruption:
            return "Corruption";
        case Status::Code::NotSupported:
            return "NotSupported";
        case Status::Code::InvalidArgument:
            return "InvalidArgument";
        case Status::Code::IOError:
            return "IOError";
        case Status::Code::Busy:
            return "Busy";
        case Status::Code::Aborted:
            return "Aborted";
    }
    return "Unknown";
}

std::string Status::to_string() const {
    if (is_ok()) return "OK";
    std::string out(code_name(code_));
    if (!msg_.empty()) {
        out.append(": ");
        out.append(msg_);
    }
    return out;
}

}  // namespace strata
