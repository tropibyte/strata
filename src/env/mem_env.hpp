// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// MemEnv: an Env that keeps files in memory, with hooks for simulating
// crashes and I/O failures in tests.
//
// Crash simulation: every file remembers how much of it had been synced.
// drop_unsynced_data() truncates each file to that length, which is what a
// power failure does to data still in the operating system's cache.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "strata/env.hpp"

namespace strata {

class MemEnv final : public Env {
public:
    MemEnv();
    ~MemEnv() override;

    Status new_sequential_file(const std::string& path, std::unique_ptr<SequentialFile>* out) override;
    Status new_random_access_file(const std::string& path, std::unique_ptr<RandomAccessFile>* out) override;
    Status new_writable_file(const std::string& path, std::unique_ptr<WritableFile>* out) override;
    Status new_appendable_file(const std::string& path, std::unique_ptr<WritableFile>* out) override;
    bool file_exists(const std::string& path) override;
    Status get_children(const std::string& dir, std::vector<std::string>* names) override;
    Status remove_file(const std::string& path) override;
    Status create_dir(const std::string& dir) override;
    Status remove_dir(const std::string& dir) override;
    Status get_file_size(const std::string& path, std::uint64_t* size) override;
    Status rename_file(const std::string& from, const std::string& to) override;
    Status sync_dir(const std::string& dir) override;
    Status lock_file(const std::string& path, std::unique_ptr<FileLock>* lock) override;
    Status unlock_file(std::unique_ptr<FileLock> lock) override;
    std::uint64_t now_micros() override;
    void sleep_for_micros(std::uint64_t micros) override;

    // ---- Test hooks -----------------------------------------------------

    /// Truncates every file to the length it had at its last sync.
    void drop_unsynced_data();

    /// While set, appends (and syncs) to writable files fail with IOError.
    void set_write_error(bool on) { write_error_.store(on); }
    /// While set, sync() fails with IOError.
    void set_sync_error(bool on) { sync_error_.store(on); }
    /// While set, creating a writable file fails with IOError.
    void set_create_error(bool on) { create_error_.store(on); }

    std::uint64_t sync_count() const { return syncs_.load(); }

    /// Direct access to a file's bytes, for corrupting it.
    bool read_raw(const std::string& path, std::string* data);
    bool write_raw(const std::string& path, std::string_view data);

    struct File;

private:
    friend class MemWritableFile;

    std::shared_ptr<File> find(const std::string& path);
    bool parent_exists(const std::string& path) const;  // requires mutex_

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<File>> files_;
    std::set<std::string> dirs_;
    std::set<std::string> locks_;

    std::atomic<bool> write_error_{false};
    std::atomic<bool> sync_error_{false};
    std::atomic<bool> create_error_{false};
    std::atomic<std::uint64_t> syncs_{0};
};

}  // namespace strata
