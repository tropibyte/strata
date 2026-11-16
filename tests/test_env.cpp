// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The Env implementations: the local file system and MemEnv.

#include <algorithm>
#include <atomic>
#include <thread>

#include "doctest.h"
#include "env/mem_env.hpp"
#include "support/test_util.hpp"

using namespace strata;

namespace {

// The same behavior is required of every Env, so the checks run against a
// directory in each.
void exercise_files(Env* env, const std::string& dir) {
    REQUIRE(env->create_dir(dir).is_ok());
    REQUIRE(env->create_dir(dir).is_ok());  // already exists: fine

    const std::string f = dir + "/f";
    CHECK_FALSE(env->file_exists(f));
    std::unique_ptr<SequentialFile> seq;
    CHECK(env->new_sequential_file(f, &seq).is_not_found());

    // Write in pieces larger and smaller than the write buffer.
    std::string expected;
    {
        std::unique_ptr<WritableFile> w;
        REQUIRE(env->new_writable_file(f, &w).is_ok());
        for (int i = 0; i < 50; ++i) {
            const std::string piece(static_cast<std::size_t>(i) * 1000 + 1, static_cast<char>('a' + i % 26));
            REQUIRE(w->append(piece).is_ok());
            expected += piece;
        }
        REQUIRE(w->append(std::string(200000, 'Z')).is_ok());
        expected += std::string(200000, 'Z');
        REQUIRE(w->sync().is_ok());
        REQUIRE(w->close().is_ok());
    }
    CHECK(env->file_exists(f));
    std::uint64_t size = 0;
    REQUIRE(env->get_file_size(f, &size).is_ok());
    CHECK(size == expected.size());

    std::string data;
    REQUIRE(read_file_to_string(env, f, &data).is_ok());
    CHECK(data == expected);

    // Sequential reads and skips.
    REQUIRE(env->new_sequential_file(f, &seq).is_ok());
    std::string chunk;
    REQUIRE(seq->read(10, chunk).is_ok());
    CHECK(chunk == expected.substr(0, 10));
    REQUIRE(seq->skip(100).is_ok());
    REQUIRE(seq->read(5, chunk).is_ok());
    CHECK(chunk == expected.substr(110, 5));
    REQUIRE(seq->skip(10000000).is_ok());
    REQUIRE(seq->read(5, chunk).is_ok());
    CHECK(chunk.empty());

    // Random reads, including past the end.
    std::unique_ptr<RandomAccessFile> ra;
    REQUIRE(env->new_random_access_file(f, &ra).is_ok());
    REQUIRE(ra->read(1000, 50, chunk).is_ok());
    CHECK(chunk == expected.substr(1000, 50));
    REQUIRE(ra->read(expected.size() - 3, 10, chunk).is_ok());
    CHECK(chunk == expected.substr(expected.size() - 3));
    REQUIRE(ra->read(expected.size() + 100, 10, chunk).is_ok());
    CHECK(chunk.empty());

    // Concurrent random reads.
    std::vector<std::thread> readers;
    std::atomic<int> mismatches{0};
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, t] {
            Random rnd(static_cast<std::uint64_t>(t) + 10);
            std::string buf;
            for (int i = 0; i < 200; ++i) {
                const std::uint64_t off = rnd.uniform(expected.size());
                if (!ra->read(off, 100, buf).is_ok() || buf != expected.substr(off, 100)) mismatches.fetch_add(1);
            }
        });
    }
    for (auto& r : readers) r.join();
    CHECK(mismatches.load() == 0);

    // Windows cannot rename or delete a file that is open.
    seq.reset();
    ra.reset();

    // Appendable files keep existing contents.
    {
        std::unique_ptr<WritableFile> a;
        REQUIRE(env->new_appendable_file(f, &a).is_ok());
        REQUIRE(a->append("tail").is_ok());
        REQUIRE(a->close().is_ok());
    }
    REQUIRE(read_file_to_string(env, f, &data).is_ok());
    CHECK(data == expected + "tail");

    // Writable files truncate.
    REQUIRE(write_string_to_file(env, "fresh", f, true).is_ok());
    REQUIRE(read_file_to_string(env, f, &data).is_ok());
    CHECK(data == "fresh");

    // Rename replaces the target.
    const std::string g = dir + "/g";
    REQUIRE(write_string_to_file(env, "gee", g, false).is_ok());
    REQUIRE(env->rename_file(f, g).is_ok());
    CHECK_FALSE(env->file_exists(f));
    REQUIRE(read_file_to_string(env, g, &data).is_ok());
    CHECK(data == "fresh");
    CHECK_FALSE(env->rename_file(f, g).is_ok());

    // Children.
    REQUIRE(write_string_to_file(env, "", dir + "/h", false).is_ok());
    std::vector<std::string> names;
    REQUIRE(env->get_children(dir, &names).is_ok());
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"g", "h"});
    CHECK_FALSE(env->get_children(dir + "/missing", &names).is_ok());

    REQUIRE(env->sync_dir(dir).is_ok());

    // Removal.
    REQUIRE(env->remove_file(g).is_ok());
    CHECK_FALSE(env->remove_file(g).is_ok());
    CHECK_FALSE(env->get_file_size(g, &size).is_ok());
    REQUIRE(env->remove_file(dir + "/h").is_ok());
    REQUIRE(env->remove_dir(dir).is_ok());
    CHECK_FALSE(env->file_exists(dir));
}

void exercise_locks(Env* env, const std::string& dir) {
    REQUIRE(env->create_dir(dir).is_ok());
    const std::string lock_path = dir + "/LOCK";
    std::unique_ptr<FileLock> lock;
    REQUIRE(env->lock_file(lock_path, &lock).is_ok());
    std::unique_ptr<FileLock> second;
    CHECK_FALSE(env->lock_file(lock_path, &second).is_ok());
    REQUIRE(env->unlock_file(std::move(lock)).is_ok());
    REQUIRE(env->lock_file(lock_path, &second).is_ok());
    REQUIRE(env->unlock_file(std::move(second)).is_ok());
}

}  // namespace

TEST_SUITE("env") {
    TEST_CASE("default env file operations") {
        test::TempDir tmp;
        exercise_files(Env::default_env(), tmp.file("work"));
    }

    TEST_CASE("mem env file operations") {
        MemEnv env;
        exercise_files(&env, "/work");
    }

    TEST_CASE("default env locks") {
        test::TempDir tmp;
        exercise_locks(Env::default_env(), tmp.path());
    }

    TEST_CASE("mem env locks") {
        MemEnv env;
        exercise_locks(&env, "/locks");
    }

    TEST_CASE("mem env refuses files in missing directories") {
        MemEnv env;
        std::unique_ptr<WritableFile> w;
        CHECK(env.new_writable_file("/nope/f", &w).is_not_found());
        REQUIRE(env.create_dir("/a/b/c").is_ok());  // creates parents
        CHECK(env.new_writable_file("/a/b/c/f", &w).is_ok());
        std::vector<std::string> names;
        REQUIRE(env.get_children("/a", &names).is_ok());
        CHECK(names == std::vector<std::string>{"b"});
    }

    TEST_CASE("mem env drops unsynced data on simulated crash") {
        MemEnv env;
        REQUIRE(env.create_dir("/d").is_ok());
        std::unique_ptr<WritableFile> w;
        REQUIRE(env.new_writable_file("/d/f", &w).is_ok());
        REQUIRE(w->append("durable").is_ok());
        REQUIRE(w->sync().is_ok());
        REQUIRE(w->append("-volatile").is_ok());
        std::string data;
        REQUIRE(read_file_to_string(&env, "/d/f", &data).is_ok());
        CHECK(data == "durable-volatile");
        env.drop_unsynced_data();
        REQUIRE(read_file_to_string(&env, "/d/f", &data).is_ok());
        CHECK(data == "durable");
        CHECK(env.sync_count() == 1);
    }

    TEST_CASE("mem env error injection") {
        MemEnv env;
        REQUIRE(env.create_dir("/d").is_ok());
        std::unique_ptr<WritableFile> w;
        REQUIRE(env.new_writable_file("/d/f", &w).is_ok());
        env.set_write_error(true);
        CHECK(w->append("x").is_io_error());
        env.set_write_error(false);
        CHECK(w->append("x").is_ok());
        env.set_sync_error(true);
        CHECK(w->sync().is_io_error());
        env.set_sync_error(false);
        env.set_create_error(true);
        CHECK(env.new_writable_file("/d/g", &w).is_io_error());
        env.set_create_error(false);

        std::string raw;
        REQUIRE(env.read_raw("/d/f", &raw));
        CHECK(raw == "x");
        REQUIRE(env.write_raw("/d/f", "replaced"));
        REQUIRE(read_file_to_string(&env, "/d/f", &raw).is_ok());
        CHECK(raw == "replaced");
        CHECK_FALSE(env.read_raw("/d/missing", &raw));
    }

    TEST_CASE("logger writes timestamped lines") {
        MemEnv env;
        REQUIRE(env.create_dir("/d").is_ok());
        {
            std::unique_ptr<Logger> logger;
            REQUIRE(env.new_logger("/d/LOG", &logger).is_ok());
            log_printf(logger.get(), "hello %d", 42);
            log_printf(logger.get(), "%s", std::string(2000, 'x').c_str());  // longer than the stack buffer
            log_printf(nullptr, "ignored %d", 1);
        }
        std::string data;
        REQUIRE(read_file_to_string(&env, "/d/LOG", &data).is_ok());
        CHECK(data.find("hello 42\n") != std::string::npos);
        CHECK(data.find(std::string(2000, 'x') + "\n") != std::string::npos);
        CHECK(std::count(data.begin(), data.end(), '\n') == 2);
        CHECK(data.substr(0, 2) == "20");  // starts with the year
    }

    TEST_CASE("clock moves forward") {
        Env* env = Env::default_env();
        const auto a = env->now_micros();
        env->sleep_for_micros(2000);
        const auto b = env->now_micros();
        CHECK(b >= a + 1000);
    }
}
