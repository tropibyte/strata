// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "env/mem_env.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace strata {

struct MemEnv::File {
    std::mutex mutex;
    std::string data;
    std::size_t synced = 0;  // bytes durable at the last sync
};

namespace {

std::string normalize(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string parent_of(const std::string& path) {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return {};
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

class MemSequentialFile final : public SequentialFile {
public:
    explicit MemSequentialFile(std::shared_ptr<MemEnv::File> file) : file_(std::move(file)) {}

    Status read(std::size_t n, std::string& result) override {
        std::lock_guard<std::mutex> lock(file_->mutex);
        if (pos_ >= file_->data.size()) {
            result.clear();
            return Status::ok();
        }
        const std::size_t len = std::min(n, file_->data.size() - pos_);
        result.assign(file_->data, pos_, len);
        pos_ += len;
        return Status::ok();
    }

    Status skip(std::uint64_t n) override {
        std::lock_guard<std::mutex> lock(file_->mutex);
        const std::uint64_t remaining = file_->data.size() - std::min<std::size_t>(pos_, file_->data.size());
        pos_ += static_cast<std::size_t>(std::min(n, remaining));
        return Status::ok();
    }

private:
    std::shared_ptr<MemEnv::File> file_;
    std::size_t pos_ = 0;
};

class MemRandomAccessFile final : public RandomAccessFile {
public:
    explicit MemRandomAccessFile(std::shared_ptr<MemEnv::File> file) : file_(std::move(file)) {}

    Status read(std::uint64_t offset, std::size_t n, std::string& result) const override {
        std::lock_guard<std::mutex> lock(file_->mutex);
        if (offset >= file_->data.size()) {
            result.clear();
            return Status::ok();
        }
        const auto off = static_cast<std::size_t>(offset);
        result.assign(file_->data, off, std::min(n, file_->data.size() - off));
        return Status::ok();
    }

private:
    std::shared_ptr<MemEnv::File> file_;
};

class MemFileLock final : public FileLock {
public:
    explicit MemFileLock(std::string p) : path(std::move(p)) {}
    std::string path;
};

}  // namespace

class MemWritableFile final : public WritableFile {
public:
    MemWritableFile(MemEnv* env, std::shared_ptr<MemEnv::File> file) : env_(env), file_(std::move(file)) {}

    Status append(std::string_view data) override {
        if (env_->write_error_.load()) return Status::io_error("simulated write error");
        std::lock_guard<std::mutex> lock(file_->mutex);
        file_->data.append(data);
        return Status::ok();
    }

    Status flush() override { return Status::ok(); }

    Status sync() override {
        if (env_->sync_error_.load() || env_->write_error_.load()) return Status::io_error("simulated sync error");
        env_->syncs_.fetch_add(1);
        std::lock_guard<std::mutex> lock(file_->mutex);
        file_->synced = file_->data.size();
        return Status::ok();
    }

    Status close() override { return Status::ok(); }

private:
    MemEnv* env_;
    std::shared_ptr<MemEnv::File> file_;
};

MemEnv::MemEnv() = default;
MemEnv::~MemEnv() = default;

std::shared_ptr<MemEnv::File> MemEnv::find(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(normalize(path));
    return it == files_.end() ? nullptr : it->second;
}

bool MemEnv::parent_exists(const std::string& path) const {
    const std::string parent = parent_of(path);
    return parent.empty() || parent == "/" || dirs_.count(parent) != 0;
}

Status MemEnv::new_sequential_file(const std::string& path, std::unique_ptr<SequentialFile>* out) {
    auto file = find(path);
    if (!file) return Status::not_found(path, "no such file");
    *out = std::make_unique<MemSequentialFile>(std::move(file));
    return Status::ok();
}

Status MemEnv::new_random_access_file(const std::string& path, std::unique_ptr<RandomAccessFile>* out) {
    auto file = find(path);
    if (!file) return Status::not_found(path, "no such file");
    *out = std::make_unique<MemRandomAccessFile>(std::move(file));
    return Status::ok();
}

Status MemEnv::new_writable_file(const std::string& path, std::unique_ptr<WritableFile>* out) {
    if (create_error_.load()) return Status::io_error(path, "simulated create error");
    const std::string p = normalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!parent_exists(p)) return Status::not_found(path, "no such directory");
    auto file = std::make_shared<File>();
    files_[p] = file;
    *out = std::make_unique<MemWritableFile>(this, std::move(file));
    return Status::ok();
}

Status MemEnv::new_appendable_file(const std::string& path, std::unique_ptr<WritableFile>* out) {
    if (create_error_.load()) return Status::io_error(path, "simulated create error");
    const std::string p = normalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!parent_exists(p)) return Status::not_found(path, "no such directory");
    auto& slot = files_[p];
    if (!slot) slot = std::make_shared<File>();
    *out = std::make_unique<MemWritableFile>(this, slot);
    return Status::ok();
}

bool MemEnv::file_exists(const std::string& path) {
    const std::string p = normalize(path);
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.count(p) != 0 || dirs_.count(p) != 0;
}

Status MemEnv::get_children(const std::string& dir, std::vector<std::string>* names) {
    names->clear();
    const std::string d = normalize(dir);
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirs_.count(d) == 0) return Status::not_found(dir, "no such directory");
    const std::string prefix = d + "/";
    auto collect = [&](const std::string& path) {
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
            path.find('/', prefix.size()) == std::string::npos) {
            names->push_back(path.substr(prefix.size()));
        }
    };
    for (const auto& [path, file] : files_) collect(path);
    for (const auto& path : dirs_) collect(path);
    return Status::ok();
}

Status MemEnv::remove_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (files_.erase(normalize(path)) == 0) return Status::not_found(path, "no such file");
    return Status::ok();
}

Status MemEnv::create_dir(const std::string& dir) {
    std::string d = normalize(dir);
    std::lock_guard<std::mutex> lock(mutex_);
    while (!d.empty() && d != "/") {
        dirs_.insert(d);
        d = parent_of(d);
    }
    return Status::ok();
}

Status MemEnv::remove_dir(const std::string& dir) {
    const std::string d = normalize(dir);
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirs_.erase(d) == 0) return Status::not_found(dir, "no such directory");
    return Status::ok();
}

Status MemEnv::get_file_size(const std::string& path, std::uint64_t* size) {
    auto file = find(path);
    if (!file) return Status::not_found(path, "no such file");
    std::lock_guard<std::mutex> lock(file->mutex);
    *size = file->data.size();
    return Status::ok();
}

Status MemEnv::rename_file(const std::string& from, const std::string& to) {
    const std::string f = normalize(from);
    const std::string t = normalize(to);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(f);
    if (it == files_.end()) return Status::not_found(from, "no such file");
    auto file = it->second;
    files_.erase(it);
    files_[t] = std::move(file);
    return Status::ok();
}

Status MemEnv::sync_dir(const std::string&) {
    return Status::ok();
}

Status MemEnv::lock_file(const std::string& path, std::unique_ptr<FileLock>* lock) {
    const std::string p = normalize(path);
    std::lock_guard<std::mutex> guard(mutex_);
    if (!parent_exists(p)) return Status::not_found(path, "no such directory");
    if (!locks_.insert(p).second) return Status::io_error("lock " + path, "already held");
    auto& slot = files_[p];
    if (!slot) slot = std::make_shared<File>();
    *lock = std::make_unique<MemFileLock>(p);
    return Status::ok();
}

Status MemEnv::unlock_file(std::unique_ptr<FileLock> lock) {
    auto* mem_lock = dynamic_cast<MemFileLock*>(lock.get());
    if (mem_lock == nullptr) return Status::invalid_argument("not a MemEnv lock");
    std::lock_guard<std::mutex> guard(mutex_);
    locks_.erase(mem_lock->path);
    return Status::ok();
}

std::uint64_t MemEnv::now_micros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void MemEnv::sleep_for_micros(std::uint64_t micros) {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

void MemEnv::drop_unsynced_data() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [path, file] : files_) {
        std::lock_guard<std::mutex> file_lock(file->mutex);
        file->data.resize(file->synced);
    }
}

bool MemEnv::read_raw(const std::string& path, std::string* data) {
    auto file = find(path);
    if (!file) return false;
    std::lock_guard<std::mutex> lock(file->mutex);
    *data = file->data;
    return true;
}

bool MemEnv::write_raw(const std::string& path, std::string_view data) {
    auto file = find(path);
    if (!file) return false;
    std::lock_guard<std::mutex> lock(file->mutex);
    file->data.assign(data);
    file->synced = file->data.size();
    return true;
}

std::unique_ptr<Env> new_mem_env() {
    return std::make_unique<MemEnv>();
}

}  // namespace strata
