// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Status: the result of an operation that can fail. Every fallible call in
// strata returns one; an OK status carries no allocation.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace strata {

class [[nodiscard]] Status {
public:
    enum class Code : std::uint8_t {
        Ok = 0,
        NotFound,
        Corruption,
        NotSupported,
        InvalidArgument,
        IOError,
        Busy,
        Aborted,
    };

    Status() noexcept = default;

    static Status ok() noexcept { return Status(); }
    static Status not_found(std::string_view msg, std::string_view detail = {}) {
        return Status(Code::NotFound, msg, detail);
    }
    static Status corruption(std::string_view msg, std::string_view detail = {}) {
        return Status(Code::Corruption, msg, detail);
    }
    static Status not_supported(std::string_view msg, std::string_view detail = {}) {
        return Status(Code::NotSupported, msg, detail);
    }
    static Status invalid_argument(std::string_view msg, std::string_view detail = {}) {
        return Status(Code::InvalidArgument, msg, detail);
    }
    static Status io_error(std::string_view msg, std::string_view detail = {}) {
        return Status(Code::IOError, msg, detail);
    }
    static Status busy(std::string_view msg, std::string_view detail = {}) { return Status(Code::Busy, msg, detail); }
    static Status aborted(std::string_view msg, std::string_view detail = {}) {
        return Status(Code::Aborted, msg, detail);
    }

    bool is_ok() const noexcept { return code_ == Code::Ok; }
    bool is_not_found() const noexcept { return code_ == Code::NotFound; }
    bool is_corruption() const noexcept { return code_ == Code::Corruption; }
    bool is_not_supported() const noexcept { return code_ == Code::NotSupported; }
    bool is_invalid_argument() const noexcept { return code_ == Code::InvalidArgument; }
    bool is_io_error() const noexcept { return code_ == Code::IOError; }
    bool is_busy() const noexcept { return code_ == Code::Busy; }
    bool is_aborted() const noexcept { return code_ == Code::Aborted; }

    Code code() const noexcept { return code_; }

    /// The message without the code prefix ("" for OK).
    const std::string& message() const noexcept { return msg_; }

    /// "OK", or "<Code>: <message>".
    std::string to_string() const;

    friend bool operator==(const Status& a, const Status& b) noexcept { return a.code_ == b.code_ && a.msg_ == b.msg_; }

private:
    Status(Code code, std::string_view msg, std::string_view detail);

    Code code_ = Code::Ok;
    std::string msg_;
};

/// Name of a status code, e.g. "NotFound".
std::string_view code_name(Status::Code code) noexcept;

}  // namespace strata
