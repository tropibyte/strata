// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Env helpers shared by every implementation: the file logger, whole-file
// read/write and printf-style logging.

#include "strata/env.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>

namespace strata {
namespace {

class FileLogger final : public Logger {
public:
    explicit FileLogger(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}

    ~FileLogger() override {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)file_->close();
    }

    void log(std::string_view line) override {
        const auto now = std::chrono::system_clock::now();
        const std::time_t secs = std::chrono::system_clock::to_time_t(now);
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &secs);
#else
        gmtime_r(&secs, &tm);
#endif
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffffu;
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "%04d/%02d/%02d-%02d:%02d:%02d.%06lld %06llx ", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(micros),
                      static_cast<unsigned long long>(tid));

        std::string record(prefix);
        record.append(line);
        if (record.empty() || record.back() != '\n') record.push_back('\n');

        std::lock_guard<std::mutex> lock(mutex_);
        (void)file_->append(record);
        (void)file_->flush();
    }

private:
    std::mutex mutex_;
    std::unique_ptr<WritableFile> file_;
};

}  // namespace

Status Env::new_logger(const std::string& path, std::unique_ptr<Logger>* out) {
    std::unique_ptr<WritableFile> file;
    Status s = new_appendable_file(path, &file);
    if (!s.is_ok()) return s;
    *out = std::make_unique<FileLogger>(std::move(file));
    return Status::ok();
}

Status write_string_to_file(Env* env, std::string_view data, const std::string& path, bool sync) {
    std::unique_ptr<WritableFile> file;
    Status s = env->new_writable_file(path, &file);
    if (!s.is_ok()) return s;
    s = file->append(data);
    if (s.is_ok() && sync) s = file->sync();
    if (s.is_ok()) s = file->close();
    if (!s.is_ok()) (void)env->remove_file(path);
    return s;
}

Status read_file_to_string(Env* env, const std::string& path, std::string* data) {
    data->clear();
    std::unique_ptr<SequentialFile> file;
    Status s = env->new_sequential_file(path, &file);
    if (!s.is_ok()) return s;
    constexpr std::size_t kChunk = 64 * 1024;
    std::string chunk;
    for (;;) {
        s = file->read(kChunk, chunk);
        if (!s.is_ok()) return s;
        if (chunk.empty()) break;
        data->append(chunk);
    }
    return Status::ok();
}

void log_printf(Logger* logger, const char* format, ...) {
    if (logger == nullptr) return;
    char stack_buf[512];
    std::va_list args;
    va_start(args, format);
    std::va_list args_copy;
    va_copy(args_copy, args);
    const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), format, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return;
    }
    if (static_cast<std::size_t>(needed) < sizeof(stack_buf)) {
        va_end(args_copy);
        logger->log(std::string_view(stack_buf, static_cast<std::size_t>(needed)));
        return;
    }
    std::string heap_buf(static_cast<std::size_t>(needed) + 1, '\0');
    std::vsnprintf(heap_buf.data(), heap_buf.size(), format, args_copy);
    va_end(args_copy);
    heap_buf.resize(static_cast<std::size_t>(needed));
    logger->log(heap_buf);
}

}  // namespace strata
