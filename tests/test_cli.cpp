// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// The strata command-line tool.

#include <sstream>

#include "cli.hpp"
#include "db/filename.hpp"
#include "doctest.h"
#include "env/mem_env.hpp"

using namespace strata;

namespace {

struct Result {
    int code;
    std::string out;
    std::string err;
};

class Cli {
public:
    Cli() { REQUIRE(env_.create_dir("/data").is_ok()); }

    Result run(const std::vector<std::string>& args, const std::string& input = {}) {
        std::istringstream in(input);
        std::ostringstream out, err;
        const int code = cli::run(args, in, out, err, &env_);
        return {code, out.str(), err.str()};
    }

    MemEnv& env() { return env_; }

    std::string find_file(FileType wanted) {
        std::vector<std::string> names;
        REQUIRE(env_.get_children("/db", &names).is_ok());
        for (const auto& n : names) {
            std::uint64_t number = 0;
            FileType type;
            if (parse_file_name(n, &number, &type) && type == wanted) return "/db/" + n;
        }
        return {};
    }

private:
    MemEnv env_;
};

}  // namespace

TEST_SUITE("cli") {
    TEST_CASE("help and usage errors") {
        Cli cli;
        auto r = cli.run({"--help"});
        CHECK(r.code == 0);
        CHECK(r.out.find("usage: strata") != std::string::npos);
        r = cli.run({});
        CHECK(r.code == 2);
        CHECK(r.err.find("usage: strata") != std::string::npos);
        r = cli.run({"frobnicate", "/db"});
        CHECK(r.code == 2);
        CHECK(r.err.find("unknown command") != std::string::npos);
        r = cli.run({"--bogus", "get", "/db", "k"});
        CHECK(r.code == 2);
        r = cli.run({"put", "/db", "only-key"});
        CHECK(r.code == 2);
    }

    TEST_CASE("put, get and del") {
        Cli cli;
        CHECK(cli.run({"put", "/db", "fruit", "mango"}).code == 0);
        auto r = cli.run({"get", "/db", "fruit"});
        CHECK(r.code == 0);
        CHECK(r.out == "mango\n");
        CHECK(cli.run({"del", "/db", "fruit"}).code == 0);
        r = cli.run({"get", "/db", "fruit"});
        CHECK(r.code == 1);
        CHECK(r.err.find("not found") != std::string::npos);
    }

    TEST_CASE("get on a missing database fails without creating it") {
        Cli cli;
        auto r = cli.run({"get", "/nothing", "k"});
        CHECK(r.code == 1);
        CHECK(r.err.find("does not exist") != std::string::npos);
        CHECK_FALSE(cli.env().file_exists("/nothing/CURRENT"));
    }

    TEST_CASE("escaped and binary keys and values") {
        Cli cli;
        CHECK(cli.run({"--escape", "put", "/db", "bin\\x00key", "v\\xffal"}).code == 0);
        auto r = cli.run({"--escape", "get", "/db", "bin\\x00key"});
        CHECK(r.out == "v\\xffal\n");
        r = cli.run({"--escape", "--hex", "get", "/db", "bin\\x00key"});
        CHECK(r.out == "76ff616c\n");
        r = cli.run({"--escape", "get", "/db", "bad\\q"});
        CHECK(r.code == 2);
        r = cli.run({"scan", "/db"});
        CHECK(r.out == "bin\\x00key\tv\\xffal\n");
    }

    TEST_CASE("scan with ranges, prefixes, limits and reverse") {
        Cli cli;
        std::string input;
        for (const char* k : {"apple", "apricot", "banana", "blueberry", "cherry"})
            input += std::string(k) + "\t" + k + "-v\n";
        auto r = cli.run({"load", "/db", "-"}, input);
        CHECK(r.code == 0);
        CHECK(r.out == "loaded 5 entries\n");

        r = cli.run({"scan", "/db", "--keys-only"});
        CHECK(r.out == "apple\napricot\nbanana\nblueberry\ncherry\n");
        r = cli.run({"scan", "/db", "--prefix", "b", "--keys-only"});
        CHECK(r.out == "banana\nblueberry\n");
        r = cli.run({"scan", "/db", "--from", "apricot", "--to", "blueberry", "--keys-only"});
        CHECK(r.out == "apricot\nbanana\n");
        r = cli.run({"scan", "/db", "--limit", "2", "--keys-only"});
        CHECK(r.out == "apple\napricot\n");
        r = cli.run({"scan", "/db", "--reverse", "--keys-only"});
        CHECK(r.out == "cherry\nblueberry\nbanana\napricot\napple\n");
        r = cli.run({"scan", "/db", "--reverse", "--prefix", "ap", "--keys-only"});
        CHECK(r.out == "apricot\napple\n");
        r = cli.run({"scan", "/db", "--reverse", "--to", "banana", "--limit", "1", "--keys-only"});
        CHECK(r.out == "apricot\n");
        r = cli.run({"scan", "/db", "--count"});
        CHECK(r.out == "5\n");
        r = cli.run({"scan", "/db", "--prefix", "c"});
        CHECK(r.out == "cherry\tcherry-v\n");
        r = cli.run({"scan", "/db", "--wat"});
        CHECK(r.code == 2);
    }

    TEST_CASE("load rejects malformed lines, export round-trips") {
        Cli cli;
        auto r = cli.run({"load", "/db", "-"}, "no tab here\n");
        CHECK(r.code == 1);
        CHECK(r.err.find("line 1") != std::string::npos);

        r = cli.run({"--escape", "load", "/db", "-"}, "k\\x01\tv\\x02\r\n\nplain\tvalue\n");
        CHECK(r.code == 0);
        r = cli.run({"export", "/db"});
        CHECK(r.code == 0);
        CHECK(r.out == "k\\x01\tv\\x02\nplain\tvalue\n");

        // What export writes, load --escape reads back.
        const std::string exported = r.out;
        CHECK(cli.run({"destroy", "/db"}).code == 0);
        CHECK(cli.run({"--escape", "load", "/db", "-"}, exported).code == 0);
        CHECK(cli.run({"export", "/db"}).out == exported);
    }

    TEST_CASE("compact, stats, property, size and check") {
        Cli cli;
        std::string input;
        for (int i = 0; i < 500; ++i) input += "key" + std::to_string(1000 + i) + "\t" + std::string(200, 'v') + "\n";
        REQUIRE(cli.run({"--write-buffer", "16K", "load", "/db", "-"}, input).code == 0);

        auto r = cli.run({"compact", "/db"});
        CHECK(r.code == 0);
        CHECK(r.out.find("compacted: files[") == 0);

        r = cli.run({"stats", "/db"});
        CHECK(r.code == 0);
        CHECK(r.out.find("Level") != std::string::npos);

        r = cli.run({"property", "/db", "strata.num-files-at-level0"});
        CHECK(r.out == "0\n");
        r = cli.run({"property", "/db", "strata.nope"});
        CHECK(r.code == 1);

        r = cli.run({"size", "/db", "key1000", "key1500"});
        CHECK(r.code == 0);
        CHECK(r.out.find(" bytes (") != std::string::npos);
        CHECK(std::stoull(r.out) > 1000);  // the values compress well

        r = cli.run({"check", "/db"});
        CHECK(r.code == 0);
        CHECK(r.out.find("0 problems") != std::string::npos);
        CHECK(r.out.find("500 entries") != std::string::npos);

        r = cli.run({"compact", "/db", "--from", "key1100", "--to", "key1200"});
        CHECK(r.code == 0);
        r = cli.run({"compact", "/db", "--sideways"});
        CHECK(r.code == 2);
    }

    TEST_CASE("check reports a damaged table") {
        Cli cli;
        std::string input;
        for (int i = 0; i < 200; ++i) input += "k" + std::to_string(i) + "\t" + std::string(100, 'x') + "\n";
        REQUIRE(cli.run({"--no-compression", "load", "/db", "-"}, input).code == 0);
        REQUIRE(cli.run({"compact", "/db"}).code == 0);
        const std::string table = cli.find_file(FileType::Table);
        REQUIRE_FALSE(table.empty());
        std::string raw;
        REQUIRE(cli.env().read_raw(table, &raw));
        raw[50] ^= 0x10;
        REQUIRE(cli.env().write_raw(table, raw));
        auto r = cli.run({"check", "/db"});
        CHECK(r.code == 1);
        CHECK(r.out.find("checksum") != std::string::npos);
    }

    TEST_CASE("dump tables, logs and manifests") {
        Cli cli;
        REQUIRE(cli.run({"put", "/db", "alpha", "one"}).code == 0);
        REQUIRE(cli.run({"put", "/db", "beta", "two"}).code == 0);

        // Before a compaction the writes are in the log.
        const std::string log = cli.find_file(FileType::Log);
        REQUIRE_FALSE(log.empty());
        auto r = cli.run({"dump", log});
        CHECK(r.code == 0);
        CHECK(r.out.find("put 'beta'") != std::string::npos);

        REQUIRE(cli.run({"compact", "/db"}).code == 0);
        const std::string table = cli.find_file(FileType::Table);
        REQUIRE_FALSE(table.empty());
        r = cli.run({"dump", table});
        CHECK(r.code == 0);
        CHECK(r.out.find("'alpha' @") != std::string::npos);
        CHECK(r.out.find("=> 'one'") != std::string::npos);
        CHECK(r.out.find("entries:") != std::string::npos);
        CHECK(r.out.find("strata.num-deletions: 0") != std::string::npos);

        r = cli.run({"dump", table, "--no-entries"});
        CHECK(r.out.find("'alpha'") == std::string::npos);
        r = cli.run({"dump", table, "--limit", "1"});
        CHECK(r.out.find("'beta'") == std::string::npos);
        CHECK(r.out.find("...") != std::string::npos);

        const std::string manifest = cli.find_file(FileType::Descriptor);
        r = cli.run({"dump", manifest});
        CHECK(r.code == 0);
        CHECK(r.out.find("AddFile") != std::string::npos);

        r = cli.run({"dump", "/db/CURRENT"});
        CHECK(r.code == 1);
        r = cli.run({"dump", "/db/notes.txt"});
        CHECK(r.code == 1);
    }

    TEST_CASE("destroy") {
        Cli cli;
        REQUIRE(cli.run({"put", "/db", "k", "v"}).code == 0);
        CHECK(cli.run({"destroy", "/db"}).code == 0);
        CHECK_FALSE(cli.env().file_exists("/db"));
        CHECK(cli.run({"get", "/db", "k"}).code == 1);
    }

    TEST_CASE("shell session") {
        Cli cli;
        const std::string script =
            "# comment lines are ignored\n"
            "put greeting \"hello world\"\n"
            "get greeting\n"
            "get missing\n"
            "put k1 v1\n"
            "snapshot\n"
            "put k1 v2\n"
            "get k1\n"
            "release\n"
            "get k1\n"
            "begin\n"
            "put b1 x\n"
            "del k1\n"
            "commit\n"
            "get k1\n"
            "get b1\n"
            "begin\n"
            "put never x\n"
            "abort\n"
            "get never\n"
            "scan k\n"
            "count\n"
            "rscan \"\" 2\n"
            "flush\n"
            "compact\n"
            "levels\n"
            "size a z\n"
            "put \"unterminated\n"
            "frob\n"
            "put onlykey\n"
            "help\n"
            "quit\n"
            "put after quit\n";
        auto r = cli.run({"shell", "/db"}, script);
        CHECK(r.code == 0);
        const std::string& o = r.out;
        CHECK(o.find("hello world\n") != std::string::npos);
        CHECK(o.find("(not found)\n") != std::string::npos);
        CHECK(o.find("reading from snapshot at sequence") != std::string::npos);
        // With the snapshot, k1 still reads v1; after release, v2.
        const auto snap_pos = o.find("reading from snapshot");
        const auto v1_pos = o.find("v1\n", snap_pos);
        const auto live_pos = o.find("reading live data");
        const auto v2_pos = o.find("v2\n", live_pos);
        CHECK(v1_pos < live_pos);
        CHECK(v2_pos != std::string::npos);
        CHECK(o.find("committed 2 updates") != std::string::npos);
        CHECK(o.find("batch discarded") != std::string::npos);
        CHECK(o.find("error: unterminated quote") != std::string::npos);
        CHECK(o.find("unknown command 'frob'") != std::string::npos);
        CHECK(o.find("usage: put <key> <value>") != std::string::npos);
        CHECK(o.find("commands:") != std::string::npos);
        CHECK(o.find("files[") != std::string::npos);
        CHECK(o.find("bytes (") != std::string::npos);
        CHECK(o.find("\n2\n") != std::string::npos);  // count: b1 and greeting
        CHECK(o.find("greeting\thello world\nb1\tx\n(limit 2 reached)") != std::string::npos);  // rscan

        // The shell's changes are in the database afterwards.
        CHECK(cli.run({"get", "/db", "b1"}).out == "x\n");
        CHECK(cli.run({"get", "/db", "k1"}).code == 1);
        CHECK(cli.run({"get", "/db", "after"}).code == 1);
    }

    TEST_CASE("split_words") {
        std::vector<std::string> w;
        REQUIRE(cli::split_words("  put  \"a b\"\tc  ", &w));
        CHECK(w == std::vector<std::string>{"put", "a b", "c"});
        REQUIRE(cli::split_words("say \"quote \\\" inside\"", &w));
        CHECK(w == std::vector<std::string>{"say", "quote \" inside"});
        REQUIRE(cli::split_words("\"\"", &w));
        CHECK(w == std::vector<std::string>{""});
        REQUIRE(cli::split_words("", &w));
        CHECK(w.empty());
        CHECK_FALSE(cli::split_words("open \"never closed", &w));
    }
}
