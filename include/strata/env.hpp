// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Env: everything strata needs from the operating system — files,
// directories, locks and the clock. The database never touches the file
// system except through an Env, so tests can run against an in-memory Env
// (new_mem_env) or one that injects faults.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "strata/status.hpp"

namespace strata {

/// A file read from start to end. Not thread-safe.
class SequentialFile {
public:
    virtual ~SequentialFile() = default;

    /// Reads up to n bytes into `result` (replacing its contents). Fewer than
    /// n bytes means end of file was reached.
    virtual Status read(std::size_t n, std::string& result) = 0;

    /// Skips n bytes. Skipping past the end is not an error.
    virtual Status skip(std::uint64_t n) = 0;
};

/// A file read at arbitrary offsets. Safe to use from several threads.
class RandomAccessFile {
public:
    virtual ~RandomAccessFile() = default;

    /// Reads up to n bytes starting at `offset` into `result` (replacing its
    /// contents). Fewer than n bytes means end of file was reached.
    virtual Status read(std::uint64_t offset, std::size_t n, std::string& result) const = 0;
};

/// A file written sequentially. Implementations buffer, so data reaches the
/// operating system on flush() and stable storage on sync(). Not thread-safe.
class WritableFile {
public:
    virtual ~WritableFile() = default;

    virtual Status append(std::string_view data) = 0;
    virtual Status flush() = 0;
    virtual Status sync() = 0;
    virtual Status close() = 0;
};

/// A held lock on a file; see Env::lock_file.
class FileLock {
public:
    virtual ~FileLock() = default;
};

/// Receives informational messages (the database's LOG file).
class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(std::string_view line) = 0;
};

class Env {
public:
    Env() = default;
    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    virtual ~Env() = default;

    /// The Env for the local file system. Lives for the whole program.
    static Env* default_env();

    virtual Status new_sequential_file(const std::string& path, std::unique_ptr<SequentialFile>* out) = 0;
    virtual Status new_random_access_file(const std::string& path, std::unique_ptr<RandomAccessFile>* out) = 0;

    /// Creates the file, or truncates it if it exists.
    virtual Status new_writable_file(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;

    /// Opens the file for appending, creating it if needed.
    virtual Status new_appendable_file(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;

    virtual bool file_exists(const std::string& path) = 0;

    /// Names (not paths) of the entries in `dir`.
    virtual Status get_children(const std::string& dir, std::vector<std::string>* names) = 0;

    virtual Status remove_file(const std::string& path) = 0;

    /// Creates the directory. Succeeds if it already exists.
    virtual Status create_dir(const std::string& dir) = 0;
    virtual Status remove_dir(const std::string& dir) = 0;

    virtual Status get_file_size(const std::string& path, std::uint64_t* size) = 0;

    /// Renames, replacing `to` if it exists.
    virtual Status rename_file(const std::string& from, const std::string& to) = 0;

    /// Makes directory entries (new and renamed files) durable. Where the
    /// platform has no such notion this does nothing.
    virtual Status sync_dir(const std::string& dir) = 0;

    /// Takes an exclusive lock on `path`, creating the file if needed, so
    /// two processes (or two DB objects) cannot open one database. Fails
    /// with IOError if the lock is held.
    virtual Status lock_file(const std::string& path, std::unique_ptr<FileLock>* lock) = 0;
    virtual Status unlock_file(std::unique_ptr<FileLock> lock) = 0;

    /// A monotonic clock, for measuring intervals.
    virtual std::uint64_t now_micros() = 0;

    virtual void sleep_for_micros(std::uint64_t micros) = 0;

    /// Opens (appending to) a log file for informational messages. Each line
    /// is prefixed with a UTC timestamp and thread id.
    virtual Status new_logger(const std::string& path, std::unique_ptr<Logger>* out);
};

/// An Env that keeps every file in memory. Locks and directories behave as on
/// disk. Files vanish with the Env.
std::unique_ptr<Env> new_mem_env();

Status write_string_to_file(Env* env, std::string_view data, const std::string& path, bool sync);
Status read_file_to_string(Env* env, const std::string& path, std::string* data);

/// printf-style logging; a null logger discards the message.
void log_printf(Logger* logger, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace strata
