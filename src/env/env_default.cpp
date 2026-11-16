// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The Env for the local file system. Directory operations use
// std::filesystem everywhere; file I/O uses POSIX descriptors (pread,
// fdatasync, fcntl locks) on Unix-like systems and the C runtime (stdio,
// _commit, share-mode locks) on Windows.

#include "strata/env.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace strata {
namespace {

constexpr std::size_t kWriteBufferSize = 64 * 1024;

Status errno_status(const std::string& context, int err) {
    if (err == ENOENT) return Status::not_found(context, std::strerror(err));
    return Status::io_error(context, std::strerror(err));
}

Status ec_status(const std::string& context, const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) return Status::not_found(context, ec.message());
    return Status::io_error(context, ec.message());
}

// Paths currently locked by this process. OS-level locks do not stop one
// process from locking a file twice, so this does.
class LockTable {
public:
    bool insert(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        return paths_.insert(path).second;
    }
    void remove(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        paths_.erase(path);
    }

private:
    std::mutex mutex_;
    std::set<std::string> paths_;
};

LockTable& lock_table() {
    static LockTable table;
    return table;
}

// ---------------------------------------------------------------------------
// Sequential reads: stdio on every platform.

class StdioSequentialFile final : public SequentialFile {
public:
    StdioSequentialFile(std::string path, std::FILE* file) : path_(std::move(path)), file_(file) {}
    ~StdioSequentialFile() override { std::fclose(file_); }

    Status read(std::size_t n, std::string& result) override {
        result.resize(n);
        const std::size_t got = std::fread(result.data(), 1, n, file_);
        result.resize(got);
        if (got < n && std::ferror(file_)) return errno_status(path_, errno);
        return Status::ok();
    }

    Status skip(std::uint64_t n) override {
#if defined(_WIN32)
        const int rc = _fseeki64(file_, static_cast<__int64>(n), SEEK_CUR);
#else
        const int rc = fseeko(file_, static_cast<off_t>(n), SEEK_CUR);
#endif
        if (rc != 0) return errno_status(path_, errno);
        return Status::ok();
    }

private:
    std::string path_;
    std::FILE* file_;
};

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows: random access through stdio under a mutex (the C runtime has no
// positional read).

class StdioRandomAccessFile final : public RandomAccessFile {
public:
    StdioRandomAccessFile(std::string path, std::FILE* file) : path_(std::move(path)), file_(file) {}
    ~StdioRandomAccessFile() override { std::fclose(file_); }

    Status read(std::uint64_t offset, std::size_t n, std::string& result) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        result.resize(n);
        if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
            result.clear();
            return errno_status(path_, errno);
        }
        const std::size_t got = std::fread(result.data(), 1, n, file_);
        result.resize(got);
        if (got < n && std::ferror(file_)) {
            std::clearerr(file_);
            return errno_status(path_, errno);
        }
        return Status::ok();
    }

private:
    std::string path_;
    mutable std::mutex mutex_;
    std::FILE* file_;
};

class StdioWritableFile final : public WritableFile {
public:
    StdioWritableFile(std::string path, std::FILE* file) : path_(std::move(path)), file_(file) {
        buffer_.reserve(kWriteBufferSize);
    }
    ~StdioWritableFile() override {
        if (file_ != nullptr) (void)close();
    }

    Status append(std::string_view data) override {
        if (buffer_.size() + data.size() > kWriteBufferSize) {
            Status s = flush_buffer();
            if (!s.is_ok()) return s;
        }
        if (data.size() >= kWriteBufferSize) return write_raw(data);
        buffer_.append(data);
        return Status::ok();
    }

    Status flush() override {
        Status s = flush_buffer();
        if (s.is_ok() && std::fflush(file_) != 0) s = errno_status(path_, errno);
        return s;
    }

    Status sync() override {
        Status s = flush();
        if (s.is_ok() && _commit(_fileno(file_)) != 0) s = errno_status(path_, errno);
        return s;
    }

    Status close() override {
        if (file_ == nullptr) return Status::ok();
        Status s = flush_buffer();
        if (std::fclose(file_) != 0 && s.is_ok()) s = errno_status(path_, errno);
        file_ = nullptr;
        return s;
    }

private:
    Status flush_buffer() {
        Status s = write_raw(buffer_);
        buffer_.clear();
        return s;
    }
    Status write_raw(std::string_view data) {
        if (data.empty()) return Status::ok();
        if (std::fwrite(data.data(), 1, data.size(), file_) != data.size()) return errno_status(path_, errno);
        return Status::ok();
    }

    std::string path_;
    std::FILE* file_;
    std::string buffer_;
};

class CrtFileLock final : public FileLock {
public:
    CrtFileLock(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
    ~CrtFileLock() override {
        _close(fd_);
        lock_table().remove(path_);
    }

private:
    std::string path_;
    int fd_;
};

Status open_stdio(const std::string& path, const char* mode, std::FILE** out) {
    std::FILE* f = std::fopen(path.c_str(), mode);
    if (f == nullptr) return errno_status(path, errno);
    *out = f;
    return Status::ok();
}

#else  // POSIX

class PosixRandomAccessFile final : public RandomAccessFile {
public:
    PosixRandomAccessFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
    ~PosixRandomAccessFile() override { ::close(fd_); }

    Status read(std::uint64_t offset, std::size_t n, std::string& result) const override {
        result.resize(n);
        std::size_t got = 0;
        while (got < n) {
            const ssize_t r = ::pread(fd_, result.data() + got, n - got, static_cast<off_t>(offset + got));
            if (r < 0) {
                if (errno == EINTR) continue;
                result.clear();
                return errno_status(path_, errno);
            }
            if (r == 0) break;  // end of file
            got += static_cast<std::size_t>(r);
        }
        result.resize(got);
        return Status::ok();
    }

private:
    std::string path_;
    int fd_;
};

class PosixWritableFile final : public WritableFile {
public:
    PosixWritableFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) { buffer_.reserve(kWriteBufferSize); }
    ~PosixWritableFile() override {
        if (fd_ >= 0) (void)close();
    }

    Status append(std::string_view data) override {
        if (buffer_.size() + data.size() > kWriteBufferSize) {
            Status s = flush_buffer();
            if (!s.is_ok()) return s;
        }
        if (data.size() >= kWriteBufferSize) return write_raw(data);
        buffer_.append(data);
        return Status::ok();
    }

    Status flush() override { return flush_buffer(); }

    Status sync() override {
        Status s = flush_buffer();
        if (!s.is_ok()) return s;
#if defined(__APPLE__)
        const int rc = ::fsync(fd_);
#else
        const int rc = ::fdatasync(fd_);
#endif
        if (rc != 0) return errno_status(path_, errno);
        return Status::ok();
    }

    Status close() override {
        if (fd_ < 0) return Status::ok();
        Status s = flush_buffer();
        if (::close(fd_) != 0 && s.is_ok()) s = errno_status(path_, errno);
        fd_ = -1;
        return s;
    }

private:
    Status flush_buffer() {
        Status s = write_raw(buffer_);
        buffer_.clear();
        return s;
    }

    Status write_raw(std::string_view data) {
        const char* p = data.data();
        std::size_t left = data.size();
        while (left > 0) {
            const ssize_t r = ::write(fd_, p, left);
            if (r < 0) {
                if (errno == EINTR) continue;
                return errno_status(path_, errno);
            }
            p += r;
            left -= static_cast<std::size_t>(r);
        }
        return Status::ok();
    }

    std::string path_;
    int fd_;
    std::string buffer_;
};

class PosixFileLock final : public FileLock {
public:
    PosixFileLock(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
    ~PosixFileLock() override {
        struct flock fl {};
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        (void)::fcntl(fd_, F_SETLK, &fl);
        ::close(fd_);
        lock_table().remove(path_);
    }

private:
    std::string path_;
    int fd_;
};

#endif

class DefaultEnv final : public Env {
public:
    Status new_sequential_file(const std::string& path, std::unique_ptr<SequentialFile>* out) override {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return errno_status(path, errno);
        *out = std::make_unique<StdioSequentialFile>(path, f);
        return Status::ok();
    }

    Status new_random_access_file(const std::string& path, std::unique_ptr<RandomAccessFile>* out) override {
#if defined(_WIN32)
        std::FILE* f = nullptr;
        Status s = open_stdio(path, "rb", &f);
        if (!s.is_ok()) return s;
        *out = std::make_unique<StdioRandomAccessFile>(path, f);
#else
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return errno_status(path, errno);
        *out = std::make_unique<PosixRandomAccessFile>(path, fd);
#endif
        return Status::ok();
    }

    Status new_writable_file(const std::string& path, std::unique_ptr<WritableFile>* out) override {
        return open_for_write(path, /*append=*/false, out);
    }

    Status new_appendable_file(const std::string& path, std::unique_ptr<WritableFile>* out) override {
        return open_for_write(path, /*append=*/true, out);
    }

    bool file_exists(const std::string& path) override {
        std::error_code ec;
        return fs::exists(fs::path(path), ec);
    }

    Status get_children(const std::string& dir, std::vector<std::string>* names) override {
        names->clear();
        std::error_code ec;
        fs::directory_iterator it(fs::path(dir), ec);
        if (ec) return ec_status(dir, ec);
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) return ec_status(dir, ec);
            names->push_back(it->path().filename().string());
        }
        if (ec) return ec_status(dir, ec);
        return Status::ok();
    }

    Status remove_file(const std::string& path) override {
        std::error_code ec;
        if (!fs::remove(fs::path(path), ec)) {
            if (ec) return ec_status(path, ec);
            return Status::not_found(path, "no such file");
        }
        return Status::ok();
    }

    Status create_dir(const std::string& dir) override {
        std::error_code ec;
        fs::create_directories(fs::path(dir), ec);
        if (ec) return ec_status(dir, ec);
        return Status::ok();
    }

    Status remove_dir(const std::string& dir) override {
        std::error_code ec;
        if (!fs::remove(fs::path(dir), ec)) {
            if (ec) return ec_status(dir, ec);
            return Status::not_found(dir, "no such directory");
        }
        return Status::ok();
    }

    Status get_file_size(const std::string& path, std::uint64_t* size) override {
        std::error_code ec;
        const auto n = fs::file_size(fs::path(path), ec);
        if (ec) {
            *size = 0;
            return ec_status(path, ec);
        }
        *size = static_cast<std::uint64_t>(n);
        return Status::ok();
    }

    Status rename_file(const std::string& from, const std::string& to) override {
#if defined(_WIN32)
        // std::filesystem::rename does not replace an existing file with
        // every Windows runtime (MinGW's uses _wrename). CURRENT is replaced
        // this way on every MANIFEST switch, so say exactly what we need.
        const std::wstring wfrom = fs::path(from).wstring();
        const std::wstring wto = fs::path(to).wstring();
        if (!::MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                return Status::not_found(from, "no such file");
            }
            return Status::io_error("rename " + from, "Windows error " + std::to_string(err));
        }
        return Status::ok();
#else
        std::error_code ec;
        fs::rename(fs::path(from), fs::path(to), ec);
        if (ec) return ec_status(from, ec);
        return Status::ok();
#endif
    }

    Status sync_dir(const std::string& dir) override {
#if defined(_WIN32)
        (void)dir;
        return Status::ok();
#else
        const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return errno_status(dir, errno);
        Status s;
        if (::fsync(fd) != 0 && errno != EINVAL) s = errno_status(dir, errno);
        ::close(fd);
        return s;
#endif
    }

    Status lock_file(const std::string& path, std::unique_ptr<FileLock>* lock) override {
        if (!lock_table().insert(path)) return Status::io_error("lock " + path, "already held by this process");
#if defined(_WIN32)
        int fd = -1;
        const errno_t err =
            _sopen_s(&fd, path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (err != 0 || fd < 0) {
            lock_table().remove(path);
            return Status::io_error("lock " + path, "held by another process");
        }
        *lock = std::make_unique<CrtFileLock>(path, fd);
#else
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            lock_table().remove(path);
            return errno_status("lock " + path, errno);
        }
        struct flock fl {};
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (::fcntl(fd, F_SETLK, &fl) != 0) {
            const int err = errno;
            ::close(fd);
            lock_table().remove(path);
            return Status::io_error("lock " + path, std::strerror(err));
        }
        *lock = std::make_unique<PosixFileLock>(path, fd);
#endif
        return Status::ok();
    }

    Status unlock_file(std::unique_ptr<FileLock> lock) override {
        lock.reset();
        return Status::ok();
    }

    std::uint64_t now_micros() override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }

    void sleep_for_micros(std::uint64_t micros) override {
        std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }

private:
    Status open_for_write(const std::string& path, bool append, std::unique_ptr<WritableFile>* out) {
#if defined(_WIN32)
        std::FILE* f = nullptr;
        Status s = open_stdio(path, append ? "ab" : "wb", &f);
        if (!s.is_ok()) return s;
        *out = std::make_unique<StdioWritableFile>(path, f);
#else
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
        const int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0) return errno_status(path, errno);
        *out = std::make_unique<PosixWritableFile>(path, fd);
#endif
        return Status::ok();
    }
};

}  // namespace

Env* Env::default_env() {
    // Never destroyed, so it outlives every database and static object.
    static Env* env = new DefaultEnv;
    return env;
}

}  // namespace strata
