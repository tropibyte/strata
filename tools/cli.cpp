// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "cli.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

#include "strata/comparator.hpp"
#include "strata/db.hpp"
#include "strata/dump.hpp"
#include "strata/env.hpp"
#include "util/strutil.hpp"

namespace strata::cli {
namespace {

constexpr const char* kUsage = R"(usage: strata [options] <command> [arguments]

Database commands (<db> is a directory):
  put <db> <key> <value>       store a value (creates the database)
  get <db> <key>               print the value for a key
  del <db> <key>               delete a key
  scan <db> [scan options]     print keys and values in order
  load <db> <file|->           store "key<TAB>value" lines (creates the database)
  export <db> [file|-]         write every entry as "key<TAB>value" lines
  compact <db> [--from K] [--to K]
                               compact a key range (default: everything)
  stats <db>                   per-level files, sizes and compaction totals
  property <db> <name>         print a property, e.g. strata.sstables
  size <db> <from> <to>        approximate bytes stored for [from, to)
  check <db>                   verify every table's checksums and key order
  destroy <db>                 delete the database
  shell <db>                   interactive shell (creates the database)

File commands:
  dump <file> [--no-entries] [--limit N]
                               print a table (.sst), log (.log) or MANIFEST

Scan options:
  --from K          start at the first key >= K
  --to K            stop before K
  --prefix P        only keys starting with P
  --limit N         at most N entries
  --reverse         go from the end backwards
  --keys-only       print only keys
  --count           print only the number of entries

Options:
  --create          create the database if it is missing
  --sync            sync the log on every write
  --escape          keys and values given are \xNN-escaped, as output is
  --raw             print keys and values unescaped
  --hex             print values as hex
  --no-compression  write uncompressed table blocks
  --bloom-bits N    bloom filter bits per key (0 disables)
  --block-size N    table block size (accepts K/M suffixes)
  --write-buffer N  memtable size (accepts K/M suffixes)
  -h, --help        show this help
)";

constexpr const char* kShellHelp = R"(commands:
  put <key> <value>         get <key>             del <key>
  scan [prefix] [limit]     count [prefix]        rscan [prefix] [limit]
  begin                     start a batch; put/del are queued until commit
  commit | abort            apply or discard the batch
  snapshot | release        read from a snapshot / go back to live reads
  flush                     write the memtable to a table file
  compact [from [to]]       compact a key range
  stats | levels | sstables show internal state
  property <name>           any property
  size <from> <to>          approximate bytes for [from, to)
  help                      this text
  quit | exit               leave the shell
Words with spaces need double quotes. Keys and values accept \xNN escapes.
)";

struct GlobalOptions {
    bool create = false;
    bool sync = false;
    bool escape = false;
    bool raw = false;
    bool hex = false;
    Options db;
};

struct Context {
    std::istream& in;
    std::ostream& out;
    std::ostream& err;
    Env* env;
    GlobalOptions opts;
};

bool parse_size(std::string_view s, std::uint64_t* out) {
    std::uint64_t v = 0;
    if (!consume_decimal(s, &v)) return false;
    std::uint64_t mult = 1;
    if (s == "k" || s == "K") {
        mult = 1u << 10;
    } else if (s == "m" || s == "M") {
        mult = 1u << 20;
    } else if (s == "g" || s == "G") {
        mult = 1u << 30;
    } else if (!s.empty()) {
        return false;
    }
    *out = v * mult;
    return true;
}

std::string show_key(const Context& ctx, std::string_view s) {
    return ctx.opts.raw ? std::string(s) : escape_string(s);
}

std::string show_value(const Context& ctx, std::string_view s) {
    if (ctx.opts.hex) return to_hex(s);
    return ctx.opts.raw ? std::string(s) : escape_string(s);
}

int fail(const Context& ctx, const std::string& message) {
    ctx.err << "strata: " << message << "\n";
    return 1;
}

int fail(const Context& ctx, const Status& s) {
    return fail(ctx, s.to_string());
}

int usage_error(const Context& ctx, const std::string& message) {
    ctx.err << "strata: " << message << "\n(run 'strata --help' for usage)\n";
    return 2;
}

// Decodes a key or value argument (unescaping under --escape).
bool decode_arg(const Context& ctx, const std::string& arg, std::string* out) {
    if (!ctx.opts.escape) {
        *out = arg;
        return true;
    }
    return unescape_string(arg, out);
}

Status open_db(Context& ctx, const std::string& path, bool create, std::unique_ptr<DB>* db) {
    Options options = ctx.opts.db;
    options.env = ctx.env;
    options.create_if_missing = create || ctx.opts.create;
    return DB::open(options, path, db);
}

WriteOptions write_options(const Context& ctx) {
    WriteOptions w;
    w.sync = ctx.opts.sync;
    return w;
}

// The smallest string greater than every string starting with prefix, or
// nullopt if there is none (the prefix is all 0xff bytes).
std::optional<std::string> prefix_successor(std::string prefix) {
    while (!prefix.empty()) {
        const auto last = static_cast<unsigned char>(prefix.back());
        if (last != 0xff) {
            prefix.back() = static_cast<char>(last + 1);
            return prefix;
        }
        prefix.pop_back();
    }
    return std::nullopt;
}

struct ScanSpec {
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::optional<std::string> prefix;
    std::uint64_t limit = 0;  // 0 = no limit
    bool reverse = false;
    bool keys_only = false;
    bool count_only = false;
};

// Runs a scan and prints it; returns the number of entries.
Status run_scan(Context& ctx, DB& db, const ReadOptions& ro, const ScanSpec& spec, std::uint64_t* count) {
    *count = 0;
    const Comparator* cmp = bytewise_comparator();
    std::optional<std::string> lower = spec.from;
    std::optional<std::string> upper = spec.to;
    if (spec.prefix) {
        if (!lower || cmp->compare(*lower, *spec.prefix) < 0) lower = spec.prefix;
        auto succ = prefix_successor(*spec.prefix);
        if (succ && (!upper || cmp->compare(*succ, *upper) < 0)) upper = succ;
    }

    auto it = db.new_iterator(ro);
    auto in_range = [&](std::string_view key) {
        if (lower && cmp->compare(key, *lower) < 0) return false;
        if (upper && cmp->compare(key, *upper) >= 0) return false;
        return true;
    };
    auto emit = [&](std::string_view key, std::string_view value) {
        ++*count;
        if (spec.count_only) return;
        if (spec.keys_only) {
            ctx.out << show_key(ctx, key) << "\n";
        } else {
            ctx.out << show_key(ctx, key) << "\t" << show_value(ctx, value) << "\n";
        }
    };

    if (!spec.reverse) {
        if (lower) {
            it->seek(*lower);
        } else {
            it->seek_to_first();
        }
        for (; it->valid() && in_range(it->key()); it->next()) {
            emit(it->key(), it->value());
            if (spec.limit != 0 && *count >= spec.limit) break;
        }
    } else {
        if (upper) {
            it->seek(*upper);
            if (it->valid()) {
                it->prev();
            } else {
                it->seek_to_last();
            }
        } else {
            it->seek_to_last();
        }
        for (; it->valid() && in_range(it->key()); it->prev()) {
            emit(it->key(), it->value());
            if (spec.limit != 0 && *count >= spec.limit) break;
        }
    }
    if (spec.count_only) ctx.out << *count << "\n";
    return it->status();
}

// ---- Commands -------------------------------------------------------------

int cmd_put(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 3) return usage_error(ctx, "put needs <db> <key> <value>");
    std::string key, value;
    if (!decode_arg(ctx, args[1], &key) || !decode_arg(ctx, args[2], &value)) {
        return usage_error(ctx, "bad escape sequence");
    }
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], /*create=*/true, &db);
    if (s.is_ok()) s = db->put(write_options(ctx), key, value);
    return s.is_ok() ? 0 : fail(ctx, s);
}

int cmd_get(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 2) return usage_error(ctx, "get needs <db> <key>");
    std::string key;
    if (!decode_arg(ctx, args[1], &key)) return usage_error(ctx, "bad escape sequence");
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (!s.is_ok()) return fail(ctx, s);
    std::string value;
    s = db->get(ReadOptions(), key, &value);
    if (s.is_not_found()) return fail(ctx, "not found: " + escape_string(key));
    if (!s.is_ok()) return fail(ctx, s);
    ctx.out << show_value(ctx, value) << "\n";
    return 0;
}

int cmd_del(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 2) return usage_error(ctx, "del needs <db> <key>");
    std::string key;
    if (!decode_arg(ctx, args[1], &key)) return usage_error(ctx, "bad escape sequence");
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (s.is_ok()) s = db->remove(write_options(ctx), key);
    return s.is_ok() ? 0 : fail(ctx, s);
}

int cmd_scan(Context& ctx, const std::vector<std::string>& args) {
    if (args.empty()) return usage_error(ctx, "scan needs <db>");
    ScanSpec spec;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value_of = [&](std::optional<std::string>* dst) {
            if (i + 1 >= args.size()) return false;
            std::string v;
            if (!decode_arg(ctx, args[++i], &v)) return false;
            *dst = std::move(v);
            return true;
        };
        if (a == "--from") {
            if (!value_of(&spec.from)) return usage_error(ctx, "--from needs a key");
        } else if (a == "--to") {
            if (!value_of(&spec.to)) return usage_error(ctx, "--to needs a key");
        } else if (a == "--prefix") {
            if (!value_of(&spec.prefix)) return usage_error(ctx, "--prefix needs a value");
        } else if (a == "--limit") {
            if (i + 1 >= args.size() || !parse_size(args[++i], &spec.limit)) {
                return usage_error(ctx, "--limit needs a number");
            }
        } else if (a == "--reverse") {
            spec.reverse = true;
        } else if (a == "--keys-only") {
            spec.keys_only = true;
        } else if (a == "--count") {
            spec.count_only = true;
        } else {
            return usage_error(ctx, "unknown scan option " + a);
        }
    }
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (!s.is_ok()) return fail(ctx, s);
    std::uint64_t count = 0;
    s = run_scan(ctx, *db, ReadOptions(), spec, &count);
    return s.is_ok() ? 0 : fail(ctx, s);
}

int cmd_load(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 2) return usage_error(ctx, "load needs <db> <file|->");
    std::ifstream file;
    std::istream* in = &ctx.in;
    if (args[1] != "-") {
        file.open(args[1], std::ios::binary);
        if (!file) return fail(ctx, "cannot open " + args[1]);
        in = &file;
    }
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], /*create=*/true, &db);
    if (!s.is_ok()) return fail(ctx, s);

    WriteBatch batch;
    std::string line, key, value;
    std::uint64_t loaded = 0, line_no = 0;
    while (std::getline(*in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) return fail(ctx, "line " + std::to_string(line_no) + ": no tab");
        if (!decode_arg(ctx, line.substr(0, tab), &key) || !decode_arg(ctx, line.substr(tab + 1), &value)) {
            return fail(ctx, "line " + std::to_string(line_no) + ": bad escape sequence");
        }
        batch.put(key, value);
        ++loaded;
        if (batch.approximate_size() >= (1u << 20)) {
            s = db->write(write_options(ctx), &batch);
            if (!s.is_ok()) return fail(ctx, s);
            batch.clear();
        }
    }
    if (batch.count() > 0) s = db->write(write_options(ctx), &batch);
    if (!s.is_ok()) return fail(ctx, s);
    ctx.out << "loaded " << loaded << " entries\n";
    return 0;
}

int cmd_export(Context& ctx, const std::vector<std::string>& args) {
    if (args.empty() || args.size() > 2) return usage_error(ctx, "export needs <db> [file|-]");
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (!s.is_ok()) return fail(ctx, s);
    std::ofstream file;
    std::ostream* out = &ctx.out;
    if (args.size() == 2 && args[1] != "-") {
        file.open(args[1], std::ios::binary);
        if (!file) return fail(ctx, "cannot create " + args[1]);
        out = &file;
    }
    auto it = db->new_iterator(ReadOptions());
    for (it->seek_to_first(); it->valid(); it->next()) {
        // Always escaped, so the output loads back with --escape.
        *out << escape_string(it->key()) << "\t" << escape_string(it->value()) << "\n";
    }
    s = it->status();
    return s.is_ok() ? 0 : fail(ctx, s);
}

int cmd_compact(Context& ctx, const std::vector<std::string>& args) {
    if (args.empty()) return usage_error(ctx, "compact needs <db>");
    std::optional<std::string> from, to;
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string v;
        if ((args[i] == "--from" || args[i] == "--to") && i + 1 < args.size() && decode_arg(ctx, args[i + 1], &v)) {
            (args[i] == "--from" ? from : to) = v;
            ++i;
        } else {
            return usage_error(ctx, "compact takes --from K and --to K");
        }
    }
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (s.is_ok()) {
        s = db->compact_range(from ? std::optional<std::string_view>(*from) : std::nullopt,
                              to ? std::optional<std::string_view>(*to) : std::nullopt);
    }
    if (!s.is_ok()) return fail(ctx, s);
    std::string levels;
    db->get_property("strata.levels", &levels);
    ctx.out << "compacted: " << levels << "\n";
    return 0;
}

int cmd_property(Context& ctx, const std::vector<std::string>& args, const std::string& fixed_name = {}) {
    const std::size_t want = fixed_name.empty() ? 2 : 1;
    if (args.size() != want) {
        return usage_error(ctx, fixed_name.empty() ? "property needs <db> <name>" : "needs <db>");
    }
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (!s.is_ok()) return fail(ctx, s);
    const std::string name = fixed_name.empty() ? args[1] : fixed_name;
    std::string value;
    if (!db->get_property(name, &value)) return fail(ctx, "unknown property " + name);
    ctx.out << value;
    if (value.empty() || value.back() != '\n') ctx.out << "\n";
    return 0;
}

int cmd_size(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 3) return usage_error(ctx, "size needs <db> <from> <to>");
    std::string from, to;
    if (!decode_arg(ctx, args[1], &from) || !decode_arg(ctx, args[2], &to)) {
        return usage_error(ctx, "bad escape sequence");
    }
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], false, &db);
    if (!s.is_ok()) return fail(ctx, s);
    const Range range{from, to};
    std::uint64_t size = 0;
    db->get_approximate_sizes(std::span<const Range>(&range, 1), std::span<std::uint64_t>(&size, 1));
    ctx.out << size << " bytes (" << human_bytes(size) << ")\n";
    return 0;
}

int cmd_check(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 1) return usage_error(ctx, "check needs <db>");
    Env* env = ctx.env != nullptr ? ctx.env : Env::default_env();
    Status s = check_db(args[0], env, ctx.out);
    return s.is_ok() ? 0 : fail(ctx, s);
}

int cmd_destroy(Context& ctx, const std::vector<std::string>& args) {
    if (args.size() != 1) return usage_error(ctx, "destroy needs <db>");
    Options options;
    options.env = ctx.env;
    Status s = destroy_db(args[0], options);
    return s.is_ok() ? 0 : fail(ctx, s);
}

int cmd_dump(Context& ctx, const std::vector<std::string>& args) {
    if (args.empty()) return usage_error(ctx, "dump needs <file>");
    DumpOptions options;
    options.hex = ctx.opts.hex;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--no-entries") {
            options.show_entries = false;
        } else if (args[i] == "--limit" && i + 1 < args.size() && parse_size(args[i + 1], &options.max_entries)) {
            ++i;
        } else {
            return usage_error(ctx, "unknown dump option " + args[i]);
        }
    }
    Env* env = ctx.env != nullptr ? ctx.env : Env::default_env();
    Status s = dump_file(env, args[0], options, ctx.out);
    return s.is_ok() ? 0 : fail(ctx, s);
}

// ---- Shell ----------------------------------------------------------------

class Shell {
public:
    Shell(Context& ctx, DB& db) : ctx_(ctx), db_(db) {}

    ~Shell() {
        if (snapshot_ != nullptr) db_.release_snapshot(snapshot_);
    }

    int run(bool interactive) {
        std::string line;
        for (;;) {
            if (interactive) ctx_.out << (batch_ ? "strata (batch)> " : "strata> ") << std::flush;
            if (!std::getline(ctx_.in, line)) break;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> words;
            if (!split_words(line, &words)) {
                ctx_.out << "error: unterminated quote\n";
                continue;
            }
            if (words.empty() || words[0][0] == '#') continue;
            if (words[0] == "quit" || words[0] == "exit") break;
            execute(words);
        }
        if (batch_ && batch_->count() > 0) ctx_.out << "(uncommitted batch of " << batch_->count() << " discarded)\n";
        return 0;
    }

private:
    bool arg(const std::vector<std::string>& w, std::size_t i, std::string* out) {
        if (i >= w.size()) return false;
        // The shell always accepts escapes: there is no other way to type
        // binary keys.
        return unescape_string(w[i], out);
    }

    ReadOptions read_options() const {
        ReadOptions ro;
        ro.snapshot = snapshot_;
        return ro;
    }

    void report(const Status& s) {
        if (s.is_ok()) {
            ctx_.out << "OK\n";
        } else {
            ctx_.out << "error: " << s.to_string() << "\n";
        }
    }

    void execute(const std::vector<std::string>& w) {
        const std::string& cmd = w[0];
        std::string a, b;
        if (cmd == "help") {
            ctx_.out << kShellHelp;
        } else if (cmd == "put") {
            if (w.size() != 3 || !arg(w, 1, &a) || !arg(w, 2, &b)) return bad("put <key> <value>");
            if (batch_) {
                batch_->put(a, b);
                ctx_.out << "queued\n";
            } else {
                report(db_.put(write_options(ctx_), a, b));
            }
        } else if (cmd == "del") {
            if (w.size() != 2 || !arg(w, 1, &a)) return bad("del <key>");
            if (batch_) {
                batch_->remove(a);
                ctx_.out << "queued\n";
            } else {
                report(db_.remove(write_options(ctx_), a));
            }
        } else if (cmd == "get") {
            if (w.size() != 2 || !arg(w, 1, &a)) return bad("get <key>");
            std::string value;
            Status s = db_.get(read_options(), a, &value);
            if (s.is_ok()) {
                ctx_.out << escape_string(value) << "\n";
            } else if (s.is_not_found()) {
                ctx_.out << "(not found)\n";
            } else {
                report(s);
            }
        } else if (cmd == "scan" || cmd == "rscan" || cmd == "count") {
            ScanSpec spec;
            spec.reverse = cmd == "rscan";
            spec.count_only = cmd == "count";
            if (w.size() > 1) {
                if (!arg(w, 1, &a)) return bad(cmd + " [prefix] [limit]");
                if (!a.empty()) spec.prefix = a;
            }
            if (w.size() > 2 && !parse_size(w[2], &spec.limit)) return bad(cmd + " [prefix] [limit]");
            if (cmd != "count" && spec.limit == 0) spec.limit = 100;
            std::uint64_t n = 0;
            Context scan_ctx{ctx_.in, ctx_.out, ctx_.err, ctx_.env, ctx_.opts};
            scan_ctx.opts.raw = false;
            Status s = run_scan(scan_ctx, db_, read_options(), spec, &n);
            if (!s.is_ok()) report(s);
            if (!spec.count_only && spec.limit != 0 && n == spec.limit) ctx_.out << "(limit " << n << " reached)\n";
        } else if (cmd == "begin") {
            if (batch_) return bad("a batch is already open");
            batch_ = std::make_unique<WriteBatch>();
            ctx_.out << "batch started\n";
        } else if (cmd == "commit") {
            if (!batch_) return bad("no batch is open");
            const auto n = batch_->count();
            Status s = db_.write(write_options(ctx_), batch_.get());
            batch_.reset();
            if (s.is_ok()) {
                ctx_.out << "committed " << n << " updates\n";
            } else {
                report(s);
            }
        } else if (cmd == "abort") {
            if (!batch_) return bad("no batch is open");
            batch_.reset();
            ctx_.out << "batch discarded\n";
        } else if (cmd == "snapshot") {
            if (snapshot_ != nullptr) db_.release_snapshot(snapshot_);
            snapshot_ = db_.get_snapshot();
            ctx_.out << "reading from snapshot at sequence " << snapshot_->sequence() << "\n";
        } else if (cmd == "release") {
            if (snapshot_ == nullptr) return bad("no snapshot is held");
            db_.release_snapshot(snapshot_);
            snapshot_ = nullptr;
            ctx_.out << "reading live data\n";
        } else if (cmd == "flush") {
            report(db_.flush());
        } else if (cmd == "compact") {
            std::optional<std::string> from, to;
            if (w.size() > 1) {
                if (!arg(w, 1, &a)) return bad("compact [from [to]]");
                from = a;
            }
            if (w.size() > 2) {
                if (!arg(w, 2, &b)) return bad("compact [from [to]]");
                to = b;
            }
            report(db_.compact_range(from ? std::optional<std::string_view>(*from) : std::nullopt,
                                     to ? std::optional<std::string_view>(*to) : std::nullopt));
        } else if (cmd == "stats" || cmd == "levels" || cmd == "sstables") {
            property("strata." + cmd);
        } else if (cmd == "property") {
            if (w.size() != 2) return bad("property <name>");
            property(w[1]);
        } else if (cmd == "size") {
            if (w.size() != 3 || !arg(w, 1, &a) || !arg(w, 2, &b)) return bad("size <from> <to>");
            const Range range{a, b};
            std::uint64_t size = 0;
            db_.get_approximate_sizes(std::span<const Range>(&range, 1), std::span<std::uint64_t>(&size, 1));
            ctx_.out << size << " bytes (" << human_bytes(size) << ")\n";
        } else {
            ctx_.out << "unknown command '" << cmd << "' (try 'help')\n";
        }
    }

    void property(const std::string& name) {
        std::string value;
        if (!db_.get_property(name, &value)) {
            ctx_.out << "unknown property " << name << "\n";
            return;
        }
        ctx_.out << value;
        if (value.empty() || value.back() != '\n') ctx_.out << "\n";
    }

    void bad(const std::string& message) { ctx_.out << "usage: " << message << "\n"; }

    Context& ctx_;
    DB& db_;
    std::unique_ptr<WriteBatch> batch_;
    const Snapshot* snapshot_ = nullptr;
};

int cmd_shell(Context& ctx, const std::vector<std::string>& args, bool interactive) {
    if (args.size() != 1) return usage_error(ctx, "shell needs <db>");
    std::unique_ptr<DB> db;
    Status s = open_db(ctx, args[0], /*create=*/true, &db);
    if (!s.is_ok()) return fail(ctx, s);
    if (interactive) ctx.out << "strata shell on " << args[0] << " ('help' for commands)\n";
    Shell shell(ctx, *db);
    return shell.run(interactive);
}

}  // namespace

bool split_words(const std::string& line, std::vector<std::string>* words) {
    words->clear();
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string word;
        if (line[i] == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '"') {
                    word.push_back('"');
                    i += 2;
                } else if (line[i] == '"') {
                    ++i;
                    closed = true;
                    break;
                } else {
                    word.push_back(line[i++]);
                }
            }
            if (!closed) return false;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') word.push_back(line[i++]);
        }
        words->push_back(std::move(word));
    }
    return true;
}

int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err, Env* env) {
    Context ctx{in, out, err, env, {}};

    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() < 2 || a[0] != '-') break;
        std::uint64_t n = 0;
        if (a == "-h" || a == "--help") {
            out << kUsage;
            return 0;
        } else if (a == "--create") {
            ctx.opts.create = true;
        } else if (a == "--sync") {
            ctx.opts.sync = true;
        } else if (a == "--escape") {
            ctx.opts.escape = true;
        } else if (a == "--raw") {
            ctx.opts.raw = true;
        } else if (a == "--hex") {
            ctx.opts.hex = true;
        } else if (a == "--no-compression") {
            ctx.opts.db.compression = Compression::None;
        } else if (a == "--bloom-bits" && i + 1 < args.size() && parse_size(args[i + 1], &n) && n <= 64) {
            ctx.opts.db.bloom_bits_per_key = static_cast<int>(n);
            ++i;
        } else if (a == "--block-size" && i + 1 < args.size() && parse_size(args[i + 1], &n)) {
            ctx.opts.db.block_size = static_cast<std::size_t>(n);
            ++i;
        } else if (a == "--write-buffer" && i + 1 < args.size() && parse_size(args[i + 1], &n)) {
            ctx.opts.db.write_buffer_size = static_cast<std::size_t>(n);
            ++i;
        } else {
            return usage_error(ctx, "unknown or incomplete option " + a);
        }
    }
    if (i >= args.size()) {
        err << kUsage;
        return 2;
    }

    const std::string& command = args[i];
    const std::vector<std::string> rest(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
    if (command == "put") return cmd_put(ctx, rest);
    if (command == "get") return cmd_get(ctx, rest);
    if (command == "del" || command == "delete") return cmd_del(ctx, rest);
    if (command == "scan") return cmd_scan(ctx, rest);
    if (command == "load") return cmd_load(ctx, rest);
    if (command == "export") return cmd_export(ctx, rest);
    if (command == "compact") return cmd_compact(ctx, rest);
    if (command == "stats") return cmd_property(ctx, rest, "strata.stats");
    if (command == "property") return cmd_property(ctx, rest);
    if (command == "size") return cmd_size(ctx, rest);
    if (command == "check") return cmd_check(ctx, rest);
    if (command == "destroy") return cmd_destroy(ctx, rest);
    if (command == "dump") return cmd_dump(ctx, rest);
    if (command == "shell") return cmd_shell(ctx, rest, /*interactive=*/&in == &std::cin);
    return usage_error(ctx, "unknown command " + command);
}

}  // namespace strata::cli
