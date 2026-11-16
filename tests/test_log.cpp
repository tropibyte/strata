// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The record log used by the WAL and the MANIFEST.

#include "db/log_format.hpp"
#include "db/log_reader.hpp"
#include "db/log_writer.hpp"
#include "doctest.h"
#include "env/mem_env.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/random.hpp"

using namespace strata;
using namespace strata::log;

namespace {

// A string of length n built from s repeated.
std::string big_string(const std::string& s, std::size_t n) {
    std::string r(n, '\0');
    for (std::size_t i = 0; i < n; ++i) r[i] = s[i % s.size()];
    return r;
}

std::string number_string(int n) {
    return std::to_string(n) + ".";
}

class LogHarness {
public:
    LogHarness() {
        REQUIRE(env_.create_dir("/l").is_ok());
        REQUIRE(env_.new_writable_file(path_, &dest_).is_ok());
        writer_ = std::make_unique<Writer>(dest_.get());
    }

    void write(const std::string& msg) {
        REQUIRE_FALSE(reading_);
        REQUIRE(writer_->add_record(msg).is_ok());
    }

    std::size_t written_bytes() {
        std::string raw;
        REQUIRE(env_.read_raw(path_, &raw));
        return raw.size();
    }

    std::string read() {
        if (!reading_) {
            reading_ = true;
            REQUIRE(env_.new_sequential_file(path_, &source_).is_ok());
            reader_ = std::make_unique<Reader>(source_.get(), &report_, /*checksum=*/true);
        }
        std::string record;
        if (reader_->read_record(&record)) return record;
        return "EOF";
    }

    std::uint64_t last_record_offset() const { return reader_->last_record_offset(); }

    // ---- Damage --------------------------------------------------------

    void increment_byte(std::size_t offset, int delta) {
        std::string raw;
        REQUIRE(env_.read_raw(path_, &raw));
        raw[offset] = static_cast<char>(raw[offset] + delta);
        REQUIRE(env_.write_raw(path_, raw));
    }

    void set_byte(std::size_t offset, char value) {
        std::string raw;
        REQUIRE(env_.read_raw(path_, &raw));
        raw[offset] = value;
        REQUIRE(env_.write_raw(path_, raw));
    }

    void shrink_size(std::size_t bytes) {
        std::string raw;
        REQUIRE(env_.read_raw(path_, &raw));
        REQUIRE(bytes <= raw.size());
        REQUIRE(env_.write_raw(path_, std::string_view(raw).substr(0, raw.size() - bytes)));
    }

    // Recomputes the CRC of the fragment header at offset with payload len.
    void fix_checksum(std::size_t header_offset, std::size_t len) {
        std::string raw;
        REQUIRE(env_.read_raw(path_, &raw));
        const std::uint32_t crc = crc32c::value(raw.data() + header_offset + 6, 1 + len);
        encode_fixed32(raw.data() + header_offset, crc32c::mask(crc));
        REQUIRE(env_.write_raw(path_, raw));
    }

    std::size_t dropped_bytes() const { return report_.dropped_bytes; }
    std::string report_message() const { return report_.message; }

    // Starts appending to the existing log with a new writer.
    void reopen_for_append() {
        dest_.reset();
        REQUIRE(env_.new_appendable_file(path_, &dest_).is_ok());
        writer_ = std::make_unique<Writer>(dest_.get(), written_bytes());
    }

private:
    struct ReportCollector final : public Reader::Reporter {
        std::size_t dropped_bytes = 0;
        std::string message;
        void corruption(std::size_t bytes, const Status& s) override {
            dropped_bytes += bytes;
            message += s.to_string();
        }
    };

    MemEnv env_;
    std::string path_ = "/l/log";
    std::unique_ptr<WritableFile> dest_;
    std::unique_ptr<Writer> writer_;
    bool reading_ = false;
    std::unique_ptr<SequentialFile> source_;
    std::unique_ptr<Reader> reader_;
    ReportCollector report_;
};

}  // namespace

TEST_SUITE("log") {
    TEST_CASE("empty log") {
        LogHarness h;
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("read back what was written") {
        LogHarness h;
        h.write("foo");
        h.write("bar");
        h.write("");
        h.write("xxxx");
        CHECK(h.read() == "foo");
        CHECK(h.read() == "bar");
        CHECK(h.read() == "");
        CHECK(h.read() == "xxxx");
        CHECK(h.read() == "EOF");
        CHECK(h.read() == "EOF");  // stays at EOF
    }

    TEST_CASE("many records span many blocks") {
        LogHarness h;
        for (int i = 0; i < 100000; ++i) h.write(number_string(i));
        for (int i = 0; i < 100000; ++i) REQUIRE(h.read() == number_string(i));
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("large records are fragmented") {
        LogHarness h;
        h.write("small");
        h.write(big_string("medium", 50000));
        h.write(big_string("large", 100000));
        CHECK(h.read() == "small");
        CHECK(h.read() == big_string("medium", 50000));
        CHECK(h.read() == big_string("large", 100000));
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("record filling a block exactly up to a trailer") {
        LogHarness h;
        const std::size_t n = kBlockSize - 2 * kHeaderSize;
        h.write(big_string("foo", n));
        CHECK(h.written_bytes() == kBlockSize - kHeaderSize);
        h.write("");
        h.write("bar");
        CHECK(h.read() == big_string("foo", n));
        CHECK(h.read() == "");
        CHECK(h.read() == "bar");
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("short trailer is zero-filled") {
        LogHarness h;
        const std::size_t n = kBlockSize - 2 * kHeaderSize + 4;
        h.write(big_string("foo", n));
        CHECK(h.written_bytes() == kBlockSize - kHeaderSize + 4);
        h.write("");
        h.write("bar");
        CHECK(h.read() == big_string("foo", n));
        CHECK(h.read() == "");
        CHECK(h.read() == "bar");
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("log ending exactly at a block boundary") {
        LogHarness h;
        const std::size_t n = kBlockSize - kHeaderSize;
        h.write(big_string("foo", n));
        CHECK(h.written_bytes() == kBlockSize);
        CHECK(h.read() == big_string("foo", n));
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("random record sizes") {
        LogHarness h;
        const int n = 500;
        Random write_rnd(301);
        for (int i = 0; i < n; ++i) h.write(big_string(number_string(i), write_rnd.skewed(17)));
        Random read_rnd(301);
        for (int i = 0; i < n; ++i) REQUIRE(h.read() == big_string(number_string(i), read_rnd.skewed(17)));
        CHECK(h.read() == "EOF");
    }

    TEST_CASE("last record offsets") {
        LogHarness h;
        h.write("a");
        h.write(big_string("b", 40000));
        h.write("c");
        CHECK(h.read() == "a");
        CHECK(h.last_record_offset() == 0);
        CHECK(h.read() == big_string("b", 40000));
        CHECK(h.last_record_offset() == kHeaderSize + 1);
        CHECK(h.read() == "c");
        CHECK(h.last_record_offset() > kBlockSize);
    }

    TEST_CASE("appending to an existing log continues the block layout") {
        LogHarness h;
        h.write("hello");
        h.write(big_string("x", 30000));
        h.reopen_for_append();
        h.write(big_string("y", 5000));
        h.write("world");
        CHECK(h.read() == "hello");
        CHECK(h.read() == big_string("x", 30000));
        CHECK(h.read() == big_string("y", 5000));
        CHECK(h.read() == "world");
        CHECK(h.read() == "EOF");
        CHECK(h.dropped_bytes() == 0);
    }

    TEST_CASE("unknown record type is reported and skipped") {
        LogHarness h;
        h.write("foo");
        h.write("bar");
        h.increment_byte(6, 100);  // type of the first fragment
        h.fix_checksum(0, 3);
        CHECK(h.read() == "bar");
        CHECK(h.read() == "EOF");
        CHECK(h.dropped_bytes() == 3);
        CHECK(h.report_message().find("unknown record type") != std::string::npos);
    }

    TEST_CASE("truncated last record reads as EOF without complaint") {
        LogHarness h;
        h.write("foo");
        h.shrink_size(1);
        CHECK(h.read() == "EOF");
        CHECK(h.dropped_bytes() == 0);
    }

    TEST_CASE("truncated header at the end reads as EOF") {
        LogHarness h;
        h.write("foo");
        h.write("bar");
        h.shrink_size(3 + 4);  // second record's payload and part of its header
        CHECK(h.read() == "foo");
        CHECK(h.read() == "EOF");
        CHECK(h.dropped_bytes() == 0);
    }

    TEST_CASE("bad length in the middle of a block is reported") {
        LogHarness h;
        const std::size_t payload = kBlockSize - kHeaderSize;
        h.write(big_string("bar", payload));
        h.write("foo");
        // Make the first record's length exceed the block.
        h.increment_byte(4, 1);
        CHECK(h.read() == "foo");
        CHECK(h.dropped_bytes() == kBlockSize);
        CHECK(h.report_message().find("bad record length") != std::string::npos);
    }

    TEST_CASE("checksum mismatch drops the rest of the block") {
        LogHarness h;
        h.write("foooooo");
        h.write("bar");
        h.increment_byte(0, 10);  // first CRC
        CHECK(h.read() == "EOF");
        CHECK(h.dropped_bytes() == 2 * kHeaderSize + 7 + 3);
        CHECK(h.report_message().find("checksum mismatch") != std::string::npos);
    }

    TEST_CASE("fragment types out of order are reported") {
        SUBCASE("middle without first") {
            LogHarness h;
            h.write("foo");
            h.set_byte(6, static_cast<char>(kMiddleType));
            h.fix_checksum(0, 3);
            CHECK(h.read() == "EOF");
            CHECK(h.dropped_bytes() == 3);
            CHECK(h.report_message().find("missing start") != std::string::npos);
        }
        SUBCASE("last without first") {
            LogHarness h;
            h.write("foo");
            h.set_byte(6, static_cast<char>(kLastType));
            h.fix_checksum(0, 3);
            CHECK(h.read() == "EOF");
            CHECK(h.dropped_bytes() == 3);
        }
        SUBCASE("first without last, then a full record") {
            LogHarness h;
            h.write("foo");
            h.write("bar");
            h.set_byte(6, static_cast<char>(kFirstType));
            h.fix_checksum(0, 3);
            CHECK(h.read() == "bar");
            CHECK(h.dropped_bytes() == 3);
            CHECK(h.report_message().find("partial record without end") != std::string::npos);
        }
        SUBCASE("first without last at the end is a torn write") {
            LogHarness h;
            h.write("foo");
            h.set_byte(6, static_cast<char>(kFirstType));
            h.fix_checksum(0, 3);
            CHECK(h.read() == "EOF");
            CHECK(h.dropped_bytes() == 0);
        }
    }

    TEST_CASE("damage inside a fragmented record drops only that record") {
        LogHarness h;
        h.write("before");
        h.write(big_string("x", 3 * kBlockSize));  // spans four blocks
        h.write("after");
        // Damage the second block (a middle fragment).
        h.increment_byte(kBlockSize + kHeaderSize + 10, 1);
        CHECK(h.read() == "before");
        CHECK(h.read() == "after");
        CHECK(h.read() == "EOF");
        CHECK(h.dropped_bytes() > 0);
    }
}
