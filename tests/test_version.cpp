// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Version edits, file-range searches and file names.

#include "db/filename.hpp"
#include "db/version_edit.hpp"
#include "db/version_set.hpp"
#include "doctest.h"
#include "env/mem_env.hpp"
#include "strata/comparator.hpp"

using namespace strata;

namespace {

void check_round_trip(const VersionEdit& edit) {
    std::string encoded, encoded2;
    edit.encode_to(encoded);
    VersionEdit parsed;
    REQUIRE(parsed.decode_from(encoded).is_ok());
    parsed.encode_to(encoded2);
    CHECK(encoded == encoded2);
    CHECK(parsed.debug_string() == edit.debug_string());
}

class FileSet {
public:
    FileSet() : icmp_(bytewise_comparator()) {}

    void add(std::string_view smallest, std::string_view largest, SequenceNumber smallest_seq = 100,
             SequenceNumber largest_seq = 100) {
        auto f = std::make_shared<FileMetaData>();
        f->number = files_.size() + 1;
        f->smallest = InternalKey(smallest, smallest_seq, ValueType::Value);
        f->largest = InternalKey(largest, largest_seq, ValueType::Value);
        files_.push_back(f);
    }

    std::size_t find(std::string_view key) const {
        const InternalKey target(key, 100, ValueType::Value);
        return find_file(icmp_, files_, target.encode());
    }

    bool overlaps(const char* smallest, const char* largest, bool disjoint = true) const {
        std::string_view s, l;
        if (smallest != nullptr) s = smallest;
        if (largest != nullptr) l = largest;
        return some_file_overlaps_range(icmp_, disjoint, files_, smallest != nullptr ? &s : nullptr,
                                        largest != nullptr ? &l : nullptr);
    }

private:
    InternalKeyComparator icmp_;
    std::vector<FileMetaPtr> files_;
};

}  // namespace

TEST_SUITE("version") {
    TEST_CASE("version edit encoding round-trips") {
        constexpr std::uint64_t kBig = std::uint64_t{1} << 50;
        VersionEdit edit;
        for (int i = 0; i < 4; ++i) {
            check_round_trip(edit);
            const auto u = static_cast<std::uint64_t>(i);
            edit.add_file(3, kBig + 300 + u, kBig + 400 + u, InternalKey("foo", kBig + 500 + u, ValueType::Value),
                          InternalKey("zoo", kBig + 600 + u, ValueType::Deletion));
            edit.remove_file(4, kBig + 700 + u);
            edit.set_compact_pointer(i, InternalKey("x", kBig + 900 + u, ValueType::Value));
        }
        edit.set_comparator_name("foo");
        edit.set_log_number(kBig + 100);
        edit.set_next_file(kBig + 200);
        edit.set_last_sequence(kBig + 1000);
        check_round_trip(edit);

        std::string encoded;
        edit.encode_to(encoded);
        VersionEdit parsed;
        REQUIRE(parsed.decode_from(encoded).is_ok());
        CHECK(parsed.comparator() == "foo");
        CHECK(parsed.log_number() == kBig + 100);
        CHECK(parsed.next_file_number() == kBig + 200);
        CHECK(parsed.last_sequence() == kBig + 1000);
        CHECK(parsed.new_files().size() == 4);
        CHECK(parsed.deleted_files().size() == 4);
        CHECK(parsed.compact_pointers().size() == 4);
        CHECK(parsed.new_files()[0].second.smallest.user_key() == "foo");
    }

    TEST_CASE("version edit clear resets everything") {
        VersionEdit edit;
        edit.set_log_number(5);
        edit.add_file(0, 1, 2, InternalKey("a", 1, ValueType::Value), InternalKey("b", 1, ValueType::Value));
        edit.clear();
        CHECK_FALSE(edit.has_log_number());
        CHECK(edit.new_files().empty());
        std::string encoded;
        edit.encode_to(encoded);
        CHECK(encoded.empty());
    }

    TEST_CASE("version edit rejects damaged encodings") {
        VersionEdit edit;
        edit.set_log_number(7);
        edit.add_file(1, 9, 1000, InternalKey("a", 1, ValueType::Value), InternalKey("z", 2, ValueType::Value));
        std::string encoded;
        edit.encode_to(encoded);
        VersionEdit parsed;
        for (std::size_t len = 1; len < encoded.size(); ++len) {
            // Some prefixes happen to end on a field boundary; the rest must fail.
            const Status s = parsed.decode_from(std::string_view(encoded.data(), len));
            if (!s.is_ok()) CHECK(s.is_corruption());
        }
        CHECK(parsed.decode_from(std::string_view(encoded.data(), encoded.size() - 1)).is_corruption());
        CHECK(parsed.decode_from("\x63").is_corruption());  // unknown tag 99

        // A level out of range is damage, not a reason to allocate.
        std::string bad_level;
        bad_level.push_back(6);    // deleted file
        bad_level.push_back(100);  // level 100
        bad_level.push_back(1);
        CHECK(parsed.decode_from(bad_level).is_corruption());
    }

    TEST_CASE("find_file on an empty level") {
        FileSet files;
        CHECK(files.find("foo") == 0);
        CHECK_FALSE(files.overlaps("a", "z"));
        CHECK_FALSE(files.overlaps(nullptr, "z"));
        CHECK_FALSE(files.overlaps("a", nullptr));
        CHECK_FALSE(files.overlaps(nullptr, nullptr));
    }

    TEST_CASE("find_file with one file") {
        FileSet files;
        files.add("p", "q");
        CHECK(files.find("a") == 0);
        CHECK(files.find("p") == 0);
        CHECK(files.find("p1") == 0);
        CHECK(files.find("q") == 0);
        CHECK(files.find("q1") == 1);
        CHECK(files.find("z") == 1);

        CHECK_FALSE(files.overlaps("a", "b"));
        CHECK_FALSE(files.overlaps("z1", "z2"));
        CHECK(files.overlaps("a", "p"));
        CHECK(files.overlaps("a", "q"));
        CHECK(files.overlaps("a", "z"));
        CHECK(files.overlaps("p", "p1"));
        CHECK(files.overlaps("p", "q"));
        CHECK(files.overlaps("p", "z"));
        CHECK(files.overlaps("p1", "p2"));
        CHECK(files.overlaps("p1", "z"));
        CHECK(files.overlaps("q", "q"));
        CHECK(files.overlaps("q", "q1"));

        CHECK_FALSE(files.overlaps(nullptr, "j"));
        CHECK_FALSE(files.overlaps("r", nullptr));
        CHECK(files.overlaps(nullptr, "p"));
        CHECK(files.overlaps(nullptr, "p1"));
        CHECK(files.overlaps("q", nullptr));
        CHECK(files.overlaps(nullptr, nullptr));
    }

    TEST_CASE("find_file with several files") {
        FileSet files;
        files.add("150", "200");
        files.add("200", "250", 99, 99);  // shares a user key with the previous file
        files.add("300", "350");
        files.add("400", "450");
        CHECK(files.find("100") == 0);
        CHECK(files.find("150") == 0);
        CHECK(files.find("151") == 0);
        CHECK(files.find("199") == 0);
        CHECK(files.find("200") == 0);
        CHECK(files.find("201") == 1);
        CHECK(files.find("249") == 1);
        CHECK(files.find("250") == 1);
        CHECK(files.find("251") == 2);
        CHECK(files.find("299") == 2);
        CHECK(files.find("300") == 2);
        CHECK(files.find("349") == 2);
        CHECK(files.find("350") == 2);
        CHECK(files.find("351") == 3);
        CHECK(files.find("400") == 3);
        CHECK(files.find("450") == 3);
        CHECK(files.find("451") == 4);

        CHECK_FALSE(files.overlaps("100", "149"));
        CHECK_FALSE(files.overlaps("251", "299"));
        CHECK_FALSE(files.overlaps("451", "500"));
        CHECK_FALSE(files.overlaps("351", "399"));
        CHECK(files.overlaps("100", "150"));
        CHECK(files.overlaps("100", "200"));
        CHECK(files.overlaps("100", "300"));
        CHECK(files.overlaps("100", "400"));
        CHECK(files.overlaps("100", "500"));
        CHECK(files.overlaps("375", "400"));
        CHECK(files.overlaps("450", "450"));
        CHECK(files.overlaps("450", "500"));
    }

    TEST_CASE("overlap checks on overlapping (level-0) files") {
        FileSet files;
        files.add("150", "600");
        files.add("400", "500");
        CHECK_FALSE(files.overlaps("100", "149", false));
        CHECK_FALSE(files.overlaps("601", "700", false));
        CHECK(files.overlaps("100", "150", false));
        CHECK(files.overlaps("100", "200", false));
        CHECK(files.overlaps("450", "700", false));
        CHECK(files.overlaps("600", "700", false));
        CHECK(files.overlaps(nullptr, nullptr, false));
    }

    TEST_CASE("file names are generated and parsed") {
        std::uint64_t number = 0;
        FileType type;
        struct Case {
            const char* name;
            std::uint64_t number;
            FileType type;
        };
        const Case cases[] = {
            {"100.log", 100, FileType::Log},
            {"0.log", 0, FileType::Log},
            {"0.sst", 0, FileType::Table},
            {"000123.sst", 123, FileType::Table},
            {"CURRENT", 0, FileType::Current},
            {"LOCK", 0, FileType::DbLock},
            {"MANIFEST-2", 2, FileType::Descriptor},
            {"MANIFEST-7", 7, FileType::Descriptor},
            {"LOG", 0, FileType::InfoLog},
            {"LOG.old", 0, FileType::InfoLog},
            {"18446744073709551615.log", 18446744073709551615ull, FileType::Log},
            {"42.dbtmp", 42, FileType::Temp},
        };
        for (const auto& c : cases) {
            CAPTURE(c.name);
            REQUIRE(parse_file_name(c.name, &number, &type));
            CHECK(number == c.number);
            CHECK(type == c.type);
        }

        for (const char* bad : {"",
                                "foo",
                                "foo-dx-100.log",
                                ".log",
                                "manifest",
                                "CURREN",
                                "CURRENTX",
                                "MANIFES",
                                "MANIFEST",
                                "MANIFEST-",
                                "XMANIFEST-3",
                                "MANIFEST-3x",
                                "LOC",
                                "LOCKx",
                                "LO",
                                "LOGx",
                                "18446744073709551616.log",
                                "184467440737095516150.log",
                                "100",
                                "100.",
                                "100.lop",
                                "100.sstx"}) {
            CAPTURE(bad);
            CHECK_FALSE(parse_file_name(bad, &number, &type));
        }

        CHECK(log_file_name("/db", 192) == "/db/000192.log");
        CHECK(table_file_name("/db", 200) == "/db/000200.sst");
        CHECK(descriptor_file_name("/db", 100) == "/db/MANIFEST-000100");
        CHECK(temp_file_name("/db", 999) == "/db/000999.dbtmp");
        CHECK(current_file_name("/db") == "/db/CURRENT");
        CHECK(lock_file_name("/db") == "/db/LOCK");
        CHECK(info_log_file_name("/db") == "/db/LOG");
        CHECK(old_info_log_file_name("/db") == "/db/LOG.old");
        CHECK(table_file_name("/db", 1234567) == "/db/1234567.sst");

        for (const std::string& path :
             {log_file_name("d", 7), table_file_name("d", 8), descriptor_file_name("d", 9), temp_file_name("d", 10)}) {
            REQUIRE(parse_file_name(path.substr(2), &number, &type));
        }
    }

    TEST_CASE("set_current_file points CURRENT at a manifest") {
        MemEnv env;
        REQUIRE(env.create_dir("/db").is_ok());
        REQUIRE(set_current_file(&env, "/db", 12).is_ok());
        std::string current;
        REQUIRE(read_file_to_string(&env, "/db/CURRENT", &current).is_ok());
        CHECK(current == "MANIFEST-000012\n");
        CHECK_FALSE(env.file_exists("/db/000012.dbtmp"));
        REQUIRE(set_current_file(&env, "/db", 13).is_ok());
        REQUIRE(read_file_to_string(&env, "/db/CURRENT", &current).is_ok());
        CHECK(current == "MANIFEST-000013\n");

        env.set_create_error(true);
        CHECK_FALSE(set_current_file(&env, "/db", 14).is_ok());
        env.set_create_error(false);
        REQUIRE(read_file_to_string(&env, "/db/CURRENT", &current).is_ok());
        CHECK(current == "MANIFEST-000013\n");  // unchanged
    }
}
