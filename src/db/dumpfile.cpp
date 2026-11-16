// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "strata/dump.hpp"

#include <map>
#include <set>

#include "db/dbformat.hpp"
#include "db/filename.hpp"
#include "db/log_reader.hpp"
#include "db/version_edit.hpp"
#include "db/write_batch_internal.hpp"
#include "strata/comparator.hpp"
#include "table/table.hpp"
#include "util/strutil.hpp"

namespace strata {
namespace {

std::string show_value(std::string_view v, const DumpOptions& options) {
    return options.hex ? to_hex(v) : escape_string(v);
}

std::string base_name(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

class PrintingReporter final : public log::Reader::Reporter {
public:
    explicit PrintingReporter(std::ostream& out) : out_(out) {}
    void corruption(std::size_t bytes, const Status& s) override {
        out_ << "corruption: dropped " << bytes << " bytes: " << s.to_string() << "\n";
        ++count;
    }
    int count = 0;

private:
    std::ostream& out_;
};

class BatchPrinter final : public WriteBatch::Handler {
public:
    BatchPrinter(std::ostream& out, const DumpOptions& options, SequenceNumber seq)
        : out_(out), options_(options), seq_(seq) {}

    void put(std::string_view key, std::string_view value) override {
        if (show())
            out_ << "  put '" << escape_string(key) << "' @ " << seq_ << " = '" << show_value(value, options_) << "'\n";
        ++seq_;
    }
    void remove(std::string_view key) override {
        if (show()) out_ << "  del '" << escape_string(key) << "' @ " << seq_ << "\n";
        ++seq_;
    }

private:
    bool show() {
        ++printed_;
        return options_.show_entries && (options_.max_entries == 0 || printed_ <= options_.max_entries);
    }

    std::ostream& out_;
    const DumpOptions& options_;
    SequenceNumber seq_;
    std::uint64_t printed_ = 0;
};

// A comparator that accepts any comparator name, for dumping tables written
// with a custom user comparator: dump only walks keys in stored order.
class DumpComparator final : public Comparator {
public:
    explicit DumpComparator(std::string name) : name_(std::move(name)) {}
    int compare(std::string_view a, std::string_view b) const override {
        // Internal keys: only used for seeks, which dumping does not do.
        return bytewise_comparator()->compare(a, b);
    }
    std::string_view name() const override { return name_; }
    void find_shortest_separator(std::string&, std::string_view) const override {}
    void find_short_successor(std::string&) const override {}

private:
    std::string name_;
};

}  // namespace

Status dump_table(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out) {
    std::uint64_t size = 0;
    Status s = env->get_file_size(path, &size);
    if (!s.is_ok()) return s;
    std::unique_ptr<RandomAccessFile> file;
    s = env->new_random_access_file(path, &file);
    if (!s.is_ok()) return s;

    // Open once with a permissive comparator to learn the real name.
    DumpComparator any("strata.InternalKeyComparator");
    TableOptions topts;
    topts.comparator = &any;
    std::shared_ptr<Table> table;
    s = Table::open(topts, std::move(file), size, nullptr, &table);
    if (!s.is_ok()) return s;

    const TableProperties& p = table->properties();
    out << "table " << base_name(path) << " (" << human_bytes(size) << ")\n";
    if (options.show_layout) {
        out << "  comparator:        " << p.comparator << "\n";
        out << "  entries:           " << with_commas(p.num_entries) << "\n";
        out << "  data blocks:       " << with_commas(p.num_data_blocks) << " (" << p.compressed_blocks
            << " compressed)\n";
        out << "  raw keys/values:   " << human_bytes(p.raw_key_size) << " / " << human_bytes(p.raw_value_size) << "\n";
        out << "  data/index/filter: " << human_bytes(p.data_size) << " / " << human_bytes(p.index_size) << " / "
            << human_bytes(p.filter_size) << "\n";
        for (const auto& [name, value] : p.user) out << "  " << name << ": " << value << "\n";
    }

    if (options.show_entries) {
        ReadOptions ropts;
        ropts.verify_checksums = true;
        auto it = table->new_iterator(ropts);
        std::uint64_t n = 0;
        for (it->seek_to_first(); it->valid(); it->next()) {
            if (options.max_entries != 0 && n >= options.max_entries) {
                out << "  ...\n";
                break;
            }
            out << "  " << debug_internal_key(it->key()) << " => '" << show_value(it->value(), options) << "'\n";
            ++n;
        }
        s = it->status();
    }
    return s;
}

Status dump_log(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out) {
    std::unique_ptr<SequentialFile> file;
    Status s = env->new_sequential_file(path, &file);
    if (!s.is_ok()) return s;
    PrintingReporter reporter(out);
    log::Reader reader(file.get(), &reporter, true);
    std::string record;
    WriteBatch batch;
    std::uint64_t records = 0;
    out << "log " << base_name(path) << "\n";
    while (reader.read_record(&record)) {
        ++records;
        out << "--- offset " << reader.last_record_offset() << ", " << record.size() << " bytes";
        if (record.size() < WriteBatchInternal::kHeaderSize) {
            out << ": record too small\n";
            continue;
        }
        WriteBatchInternal::set_contents(batch, record);
        const SequenceNumber seq = WriteBatchInternal::sequence(batch);
        out << ", sequence " << seq << ", " << WriteBatchInternal::count(batch) << " updates\n";
        BatchPrinter printer(out, options, seq);
        Status bs = batch.iterate(printer);
        if (!bs.is_ok()) out << "  error: " << bs.to_string() << "\n";
    }
    out << records << " records";
    if (reporter.count > 0) out << ", " << reporter.count << " corrupted regions";
    out << "\n";
    return Status::ok();
}

Status dump_manifest(Env* env, const std::string& path, const DumpOptions&, std::ostream& out) {
    std::unique_ptr<SequentialFile> file;
    Status s = env->new_sequential_file(path, &file);
    if (!s.is_ok()) return s;
    PrintingReporter reporter(out);
    log::Reader reader(file.get(), &reporter, true);
    std::string record;
    std::uint64_t n = 0;
    out << "manifest " << base_name(path) << "\n";
    while (reader.read_record(&record)) {
        VersionEdit edit;
        Status es = edit.decode_from(record);
        out << "--- edit " << n++ << " (offset " << reader.last_record_offset() << ")\n";
        if (!es.is_ok()) {
            out << "  error: " << es.to_string() << "\n";
            continue;
        }
        out << edit.debug_string();
    }
    return Status::ok();
}

Status dump_file(Env* env, const std::string& path, const DumpOptions& options, std::ostream& out) {
    std::uint64_t number = 0;
    FileType type;
    if (!parse_file_name(base_name(path), &number, &type)) {
        return Status::invalid_argument(path, "not a strata file name (NNNNNN.sst, NNNNNN.log, MANIFEST-NNNNNN)");
    }
    switch (type) {
        case FileType::Table:
            return dump_table(env, path, options, out);
        case FileType::Log:
            return dump_log(env, path, options, out);
        case FileType::Descriptor:
            return dump_manifest(env, path, options, out);
        default:
            return Status::invalid_argument(path, "cannot dump this kind of file");
    }
}

Status check_db(const std::string& path, Env* env, std::ostream& out) {
    std::vector<std::string> names;
    Status s = env->get_children(path, &names);
    if (!s.is_ok()) return s;

    std::string current;
    s = read_file_to_string(env, current_file_name(path), &current);
    if (!s.is_ok()) return s;
    while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) current.pop_back();

    // Replay the MANIFEST to find the live table files.
    std::unique_ptr<SequentialFile> file;
    s = env->new_sequential_file(path + "/" + current, &file);
    if (!s.is_ok()) return s;
    PrintingReporter reporter(out);
    log::Reader reader(file.get(), &reporter, true);
    std::map<std::uint64_t, std::uint64_t> live;  // number -> size
    std::string record;
    int problems = 0;
    while (reader.read_record(&record)) {
        VersionEdit edit;
        Status es = edit.decode_from(record);
        if (!es.is_ok()) {
            out << "MANIFEST: " << es.to_string() << "\n";
            ++problems;
            continue;
        }
        for (const auto& [level, number] : edit.deleted_files()) live.erase(number);
        for (const auto& [level, f] : edit.new_files()) live[f.number] = f.file_size;
    }
    problems += reporter.count;

    DumpComparator any("strata.InternalKeyComparator");
    InternalKeyComparator icmp(bytewise_comparator());
    std::uint64_t entries = 0;
    for (const auto& [number, size] : live) {
        const std::string name = table_file_name(path, number);
        std::uint64_t actual = 0;
        Status fs = env->get_file_size(name, &actual);
        if (!fs.is_ok()) {
            out << base_name(name) << ": " << fs.to_string() << "\n";
            ++problems;
            continue;
        }
        if (actual != size) {
            out << base_name(name) << ": size " << actual << ", MANIFEST says " << size << "\n";
            ++problems;
        }
        std::unique_ptr<RandomAccessFile> rfile;
        fs = env->new_random_access_file(name, &rfile);
        std::shared_ptr<Table> table;
        TableOptions topts;
        topts.comparator = &any;
        if (fs.is_ok()) fs = Table::open(topts, std::move(rfile), actual, nullptr, &table);
        if (!fs.is_ok()) {
            out << base_name(name) << ": " << fs.to_string() << "\n";
            ++problems;
            continue;
        }
        ReadOptions ropts;
        ropts.verify_checksums = true;
        auto it = table->new_iterator(ropts);
        std::string prev;
        bool ordered = true;
        for (it->seek_to_first(); it->valid(); it->next()) {
            ++entries;
            // Order check assumes the default comparator; skip otherwise.
            if (table->properties().comparator == icmp.name() && !prev.empty() && icmp.compare(prev, it->key()) >= 0) {
                ordered = false;
            }
            prev.assign(it->key());
        }
        if (!it->status().is_ok()) {
            out << base_name(name) << ": " << it->status().to_string() << "\n";
            ++problems;
        } else if (!ordered) {
            out << base_name(name) << ": keys out of order\n";
            ++problems;
        }
    }
    out << live.size() << " table files, " << with_commas(entries) << " entries checked, " << problems << " problems\n";
    if (problems > 0) return Status::corruption("check found " + std::to_string(problems) + " problems");
    return Status::ok();
}

}  // namespace strata
