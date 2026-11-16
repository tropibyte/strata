// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "db/version_set.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

#include "db/filename.hpp"
#include "db/log_reader.hpp"
#include "db/table_cache.hpp"
#include "table/merging_iterator.hpp"
#include "table/two_level_iterator.hpp"
#include "util/coding.hpp"
#include "util/strutil.hpp"

namespace strata {
namespace {

std::uint64_t total_file_size(const std::vector<FileMetaPtr>& files) {
    std::uint64_t sum = 0;
    for (const auto& f : files) sum += f->file_size;
    return sum;
}

// Overlap limits, as multiples of the target file size.
std::uint64_t max_grandparent_overlap_bytes(const Options& options) {
    return 10 * static_cast<std::uint64_t>(options.target_file_size);
}

std::uint64_t expanded_compaction_byte_size_limit(const Options& options) {
    return 25 * static_cast<std::uint64_t>(options.target_file_size);
}

bool after_file(const Comparator* ucmp, const std::string_view* user_key, const FileMetaData& f) {
    // Null user_key occurs before all keys and is therefore never after a file.
    return user_key != nullptr && ucmp->compare(*user_key, f.largest.user_key()) > 0;
}

bool before_file(const Comparator* ucmp, const std::string_view* user_key, const FileMetaData& f) {
    // Null user_key occurs after all keys and is therefore never before a file.
    return user_key != nullptr && ucmp->compare(*user_key, f.smallest.user_key()) < 0;
}

// Iterates over the files of one sorted level (or a compaction input list):
// key = the file's largest internal key, value = fixed64 number | fixed64 size.
class LevelFileIterator final : public Iterator {
public:
    LevelFileIterator(const InternalKeyComparator& icmp, std::vector<FileMetaPtr> files)
        : icmp_(icmp), files_(std::move(files)), index_(files_.size()) {}

    bool valid() const override { return index_ < files_.size(); }
    void seek(std::string_view target) override { index_ = find_file(icmp_, files_, target); }
    void seek_to_first() override { index_ = 0; }
    void seek_to_last() override { index_ = files_.empty() ? 0 : files_.size() - 1; }
    void next() override { ++index_; }
    void prev() override { index_ = index_ == 0 ? files_.size() : index_ - 1; }

    std::string_view key() const override { return files_[index_]->largest.encode(); }

    std::string_view value() const override {
        encode_fixed64(value_buf_, files_[index_]->number);
        encode_fixed64(value_buf_ + 8, files_[index_]->file_size);
        return std::string_view(value_buf_, sizeof(value_buf_));
    }

    Status status() const override { return Status::ok(); }

private:
    const InternalKeyComparator& icmp_;
    const std::vector<FileMetaPtr> files_;
    std::size_t index_;
    mutable char value_buf_[16];
};

std::unique_ptr<Iterator> new_level_iterator(TableCache* table_cache, const InternalKeyComparator& icmp,
                                             const ReadOptions& options, std::vector<FileMetaPtr> files) {
    return new_two_level_iterator(
        std::make_unique<LevelFileIterator>(icmp, std::move(files)),
        [table_cache, options](std::string_view file_value) {
            if (file_value.size() != 16) {
                return new_empty_iterator(Status::corruption("level iterator given a bad file value"));
            }
            return table_cache->new_iterator(options, decode_fixed64(file_value.data()),
                                             decode_fixed64(file_value.data() + 8));
        });
}

class ManifestReporter final : public log::Reader::Reporter {
public:
    explicit ManifestReporter(Status* status) : status_(status) {}
    void corruption(std::size_t, const Status& s) override {
        if (status_->is_ok()) *status_ = s;
    }

private:
    Status* status_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Free functions

std::size_t find_file(const InternalKeyComparator& icmp, const std::vector<FileMetaPtr>& files, std::string_view key) {
    std::size_t left = 0;
    std::size_t right = files.size();
    while (left < right) {
        const std::size_t mid = left + (right - left) / 2;
        if (icmp.compare(files[mid]->largest.encode(), key) < 0) {
            left = mid + 1;  // everything at or before mid is < key
        } else {
            right = mid;
        }
    }
    return right;
}

bool some_file_overlaps_range(const InternalKeyComparator& icmp, bool disjoint, const std::vector<FileMetaPtr>& files,
                              const std::string_view* smallest_user_key, const std::string_view* largest_user_key) {
    const Comparator* ucmp = icmp.user_comparator();
    if (!disjoint) {
        for (const auto& f : files) {
            if (after_file(ucmp, smallest_user_key, *f) || before_file(ucmp, largest_user_key, *f)) continue;
            return true;
        }
        return false;
    }

    std::size_t index = 0;
    if (smallest_user_key != nullptr) {
        const InternalKey small(*smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        index = find_file(icmp, files, small.encode());
    }
    if (index >= files.size()) return false;  // beginning of range is after all files
    return !before_file(ucmp, largest_user_key, *files[index]);
}

// ---------------------------------------------------------------------------
// Version

Version::Version(VersionSet* vset) : vset_(vset), files_(static_cast<std::size_t>(vset->num_levels())) {}

std::uint64_t Version::level_bytes(int level) const {
    return total_file_size(files(level));
}

Status Version::get(const ReadOptions& options, const LookupKey& key, std::string* value) const {
    const InternalKeyComparator& icmp = *vset_->icmp_;
    const Comparator* ucmp = icmp.user_comparator();
    const std::string_view ikey = key.internal_key();
    const std::string_view user_key = key.user_key();

    enum class State { NotFound, Found, Deleted, Corrupt };
    std::vector<const FileMetaData*> candidates;

    for (int level = 0; level < num_levels(); ++level) {
        const auto& level_files = files(level);
        if (level_files.empty()) continue;

        candidates.clear();
        if (level == 0) {
            // Level-0 files may overlap; check every one that could hold the
            // key, newest first.
            for (const auto& f : level_files) {
                if (ucmp->compare(user_key, f->smallest.user_key()) >= 0 &&
                    ucmp->compare(user_key, f->largest.user_key()) <= 0) {
                    candidates.push_back(f.get());
                }
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const FileMetaData* a, const FileMetaData* b) { return a->number > b->number; });
        } else {
            const std::size_t index = find_file(icmp, level_files, ikey);
            if (index < level_files.size() && ucmp->compare(user_key, level_files[index]->smallest.user_key()) >= 0) {
                candidates.push_back(level_files[index].get());
            }
        }

        for (const FileMetaData* f : candidates) {
            State state = State::NotFound;
            Status s = vset_->table_cache_->get(options, f->number, f->file_size, ikey,
                                                [&](std::string_view found_key, std::string_view found_value) {
                                                    ParsedInternalKey parsed;
                                                    if (!parse_internal_key(found_key, &parsed)) {
                                                        state = State::Corrupt;
                                                    } else if (ucmp->compare(parsed.user_key, user_key) == 0) {
                                                        if (parsed.type == ValueType::Value) {
                                                            state = State::Found;
                                                            value->assign(found_value);
                                                        } else {
                                                            state = State::Deleted;
                                                        }
                                                    }
                                                });
            if (!s.is_ok()) return s;
            switch (state) {
                case State::NotFound:
                    break;  // keep looking
                case State::Found:
                    return Status::ok();
                case State::Deleted:
                    return Status::not_found("deleted");
                case State::Corrupt:
                    return Status::corruption("corrupted key for", escape_string(user_key));
            }
        }
    }
    return Status::not_found("no entry");
}

std::unique_ptr<Iterator> Version::new_concatenating_iterator(const ReadOptions& options, int level) const {
    return new_level_iterator(vset_->table_cache_, *vset_->icmp_, options, files(level));
}

void Version::add_iterators(const ReadOptions& options, std::vector<std::unique_ptr<Iterator>>* iters) const {
    for (const auto& f : files(0)) {
        iters->push_back(vset_->table_cache_->new_iterator(options, f->number, f->file_size));
    }
    for (int level = 1; level < num_levels(); ++level) {
        if (!files(level).empty()) iters->push_back(new_concatenating_iterator(options, level));
    }
}

bool Version::overlap_in_level(int level, const std::string_view* smallest_user_key,
                               const std::string_view* largest_user_key) const {
    return some_file_overlaps_range(*vset_->icmp_, level > 0, files(level), smallest_user_key, largest_user_key);
}

int Version::pick_level_for_memtable_output(std::string_view smallest_user_key,
                                            std::string_view largest_user_key) const {
    int level = 0;
    if (overlap_in_level(0, &smallest_user_key, &largest_user_key)) return level;

    // Push to the next level if nothing there overlaps, and the level after
    // that would not overlap too much.
    const InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    const InternalKey limit(largest_user_key, 0, ValueType::Deletion);
    const Options& options = vset_->options();
    const int max_level = std::min(options.max_memtable_level, num_levels() - 1);
    std::vector<FileMetaPtr> overlaps;
    while (level < max_level) {
        if (overlap_in_level(level + 1, &smallest_user_key, &largest_user_key)) break;
        if (level + 2 < num_levels()) {
            get_overlapping_inputs(level + 2, &start, &limit, &overlaps);
            if (total_file_size(overlaps) > max_grandparent_overlap_bytes(options)) break;
        }
        ++level;
    }
    return level;
}

void Version::get_overlapping_inputs(int level, const InternalKey* begin, const InternalKey* end,
                                     std::vector<FileMetaPtr>* inputs) const {
    inputs->clear();
    const Comparator* ucmp = vset_->icmp_->user_comparator();
    std::string_view user_begin, user_end;
    if (begin != nullptr) user_begin = begin->user_key();
    if (end != nullptr) user_end = end->user_key();

    const auto& level_files = files(level);
    for (std::size_t i = 0; i < level_files.size();) {
        const FileMetaPtr& f = level_files[i++];
        const std::string_view file_start = f->smallest.user_key();
        const std::string_view file_limit = f->largest.user_key();
        if (begin != nullptr && ucmp->compare(file_limit, user_begin) < 0) continue;  // entirely before range
        if (end != nullptr && ucmp->compare(file_start, user_end) > 0) continue;      // entirely after range
        inputs->push_back(f);
        if (level == 0) {
            // Level-0 files can overlap each other: if this file sticks out of
            // the range, widen the range and start over.
            if (begin != nullptr && ucmp->compare(file_start, user_begin) < 0) {
                user_begin = file_start;
                inputs->clear();
                i = 0;
            } else if (end != nullptr && ucmp->compare(file_limit, user_end) > 0) {
                user_end = file_limit;
                inputs->clear();
                i = 0;
            }
        }
    }
}

std::string Version::debug_string() const {
    std::string r;
    for (int level = 0; level < num_levels(); ++level) {
        r += "--- level " + std::to_string(level) + " ---\n";
        for (const auto& f : files(level)) {
            r += " " + std::to_string(f->number) + ":" + std::to_string(f->file_size) + "[" +
                 f->smallest.debug_string() + " .. " + f->largest.debug_string() + "]\n";
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// VersionSet::Builder: applies a sequence of edits to a base version.

class VersionSet::Builder {
public:
    Builder(VersionSet* vset, const Version* base)
        : vset_(vset), base_(base), levels_(static_cast<std::size_t>(vset->num_levels_)) {}

    Status apply(const VersionEdit& edit) {
        const int n = vset_->num_levels_;
        for (const auto& [level, key] : edit.compact_pointers()) {
            if (level >= n) return level_error(level);
            vset_->compact_pointer_[static_cast<std::size_t>(level)].assign(key.encode());
        }
        for (const auto& [level, number] : edit.deleted_files()) {
            if (level >= n) return level_error(level);
            levels_[static_cast<std::size_t>(level)].deleted.insert(number);
        }
        for (const auto& [level, f] : edit.new_files()) {
            if (level >= n) return level_error(level);
            auto& state = levels_[static_cast<std::size_t>(level)];
            state.deleted.erase(f.number);
            state.added.push_back(std::make_shared<const FileMetaData>(f));
        }
        return Status::ok();
    }

    Status save_to(Version* v) const {
        const InternalKeyComparator& icmp = *vset_->icmp_;
        for (std::size_t level = 0; level < levels_.size(); ++level) {
            const auto& state = levels_[level];
            std::vector<FileMetaPtr> files;
            auto keep = [&](const FileMetaPtr& f) {
                if (state.deleted.count(f->number) == 0) files.push_back(f);
            };
            if (base_ != nullptr) {
                for (const auto& f : base_->files_[level]) keep(f);
            }
            for (const auto& f : state.added) keep(f);

            if (level == 0) {
                std::sort(files.begin(), files.end(),
                          [](const FileMetaPtr& a, const FileMetaPtr& b) { return a->number < b->number; });
            } else {
                std::sort(files.begin(), files.end(), [&](const FileMetaPtr& a, const FileMetaPtr& b) {
                    const int r = icmp.compare(a->smallest.encode(), b->smallest.encode());
                    return r != 0 ? r < 0 : a->number < b->number;
                });
                for (std::size_t i = 1; i < files.size(); ++i) {
                    if (icmp.compare(files[i - 1]->largest.encode(), files[i]->smallest.encode()) >= 0) {
                        return Status::corruption(
                            "overlapping files in level " + std::to_string(level),
                            std::to_string(files[i - 1]->number) + " and " + std::to_string(files[i]->number));
                    }
                }
            }
            v->files_[level] = std::move(files);
        }
        return Status::ok();
    }

private:
    struct LevelState {
        std::set<std::uint64_t> deleted;
        std::vector<FileMetaPtr> added;
    };

    Status level_error(int level) const {
        return Status::invalid_argument(
            "MANIFEST uses level " + std::to_string(level),
            "the database was opened with num_levels = " + std::to_string(vset_->num_levels_));
    }

    VersionSet* vset_;
    const Version* base_;
    std::vector<LevelState> levels_;
};

// ---------------------------------------------------------------------------
// VersionSet

VersionSet::VersionSet(std::string dbname, const Options* options, TableCache* table_cache,
                       const InternalKeyComparator* icmp)
    : dbname_(std::move(dbname)),
      options_(options),
      table_cache_(table_cache),
      icmp_(icmp),
      env_(options->env),
      num_levels_(options->num_levels),
      compact_pointer_(static_cast<std::size_t>(options->num_levels)) {
    append_version(std::make_shared<Version>(this));
}

VersionSet::~VersionSet() = default;

void VersionSet::append_version(std::shared_ptr<Version> v) {
    current_ = v;
    versions_.push_back(v);
    versions_.remove_if([](const std::weak_ptr<Version>& w) { return w.expired(); });
}

double VersionSet::max_bytes_for_level(int level) const {
    double result = static_cast<double>(options_->max_bytes_for_level_base);
    for (int l = 1; l < level; ++l) result *= options_->level_size_multiplier;
    return result;
}

std::uint64_t VersionSet::max_file_size_for_level(int) const {
    return static_cast<std::uint64_t>(options_->target_file_size);
}

void VersionSet::finalize(Version* v) const {
    int best_level = -1;
    double best_score = -1;
    for (int level = 0; level + 1 < num_levels_; ++level) {
        double score;
        if (level == 0) {
            // Level 0 is scored by file count rather than bytes: each file is
            // read on every lookup, and with small write buffers the bytes
            // would understate the cost.
            score = static_cast<double>(v->num_files(0)) / static_cast<double>(options_->level0_compaction_trigger);
        } else {
            score = static_cast<double>(v->level_bytes(level)) / max_bytes_for_level(level);
        }
        if (score > best_score) {
            best_level = level;
            best_score = score;
        }
    }
    v->compaction_level_ = best_level;
    v->compaction_score_ = best_score;
}

Status VersionSet::write_snapshot(log::Writer* log) {
    VersionEdit edit;
    edit.set_comparator_name(icmp_->user_comparator()->name());
    for (int level = 0; level < num_levels_; ++level) {
        const std::string& pointer = compact_pointer_[static_cast<std::size_t>(level)];
        if (!pointer.empty()) {
            InternalKey key;
            (void)key.decode_from(pointer);
            edit.set_compact_pointer(level, key);
        }
    }
    for (int level = 0; level < num_levels_; ++level) {
        for (const auto& f : current_->files(level)) edit.add_file(level, *f);
    }
    std::string record;
    edit.encode_to(record);
    return log->add_record(record);
}

Status VersionSet::log_and_apply(VersionEdit* edit, std::unique_lock<std::mutex>& lock) {
    if (edit->has_log_number()) {
        assert(edit->log_number() >= log_number_);
        assert(edit->log_number() < next_file_number_);
    } else {
        edit->set_log_number(log_number_);
    }
    edit->set_next_file(next_file_number_);
    edit->set_last_sequence(last_sequence_);

    auto v = std::make_shared<Version>(this);
    {
        Builder builder(this, current_.get());
        Status s = builder.apply(*edit);
        if (s.is_ok()) s = builder.save_to(v.get());
        if (!s.is_ok()) return s;
    }
    finalize(v.get());

    // Open a new MANIFEST if none is open (the first edit after open). It
    // starts with a snapshot of the current state.
    std::string new_manifest;
    Status s;
    if (!descriptor_log_) {
        new_manifest = descriptor_file_name(dbname_, manifest_file_number_);
        s = env_->new_writable_file(new_manifest, &descriptor_file_);
        if (s.is_ok()) {
            descriptor_log_ = std::make_unique<log::Writer>(descriptor_file_.get());
            s = write_snapshot(descriptor_log_.get());
        }
    }

    // Write the edit without holding the mutex: it involves a sync.
    lock.unlock();
    if (s.is_ok()) {
        std::string record;
        edit->encode_to(record);
        s = descriptor_log_->add_record(record);
        if (s.is_ok()) s = descriptor_file_->sync();
    }
    if (s.is_ok() && !new_manifest.empty()) s = set_current_file(env_, dbname_, manifest_file_number_);
    lock.lock();

    if (s.is_ok()) {
        append_version(std::move(v));
        log_number_ = edit->log_number();
    } else if (!new_manifest.empty()) {
        descriptor_log_.reset();
        descriptor_file_.reset();
        (void)env_->remove_file(new_manifest);
    }
    return s;
}

Status VersionSet::recover() {
    std::string current;
    Status s = read_file_to_string(env_, current_file_name(dbname_), &current);
    if (!s.is_ok()) return s;
    if (current.empty() || current.back() != '\n') return Status::corruption("CURRENT file does not end with newline");
    current.pop_back();

    const std::string manifest_path = dbname_ + "/" + current;
    std::unique_ptr<SequentialFile> file;
    s = env_->new_sequential_file(manifest_path, &file);
    if (!s.is_ok()) {
        if (s.is_not_found()) return Status::corruption("CURRENT points to a missing file", s.message());
        return s;
    }

    bool have_log_number = false, have_next_file = false, have_last_sequence = false;
    std::uint64_t next_file = 0, log_number = 0;
    SequenceNumber last_sequence = 0;
    Builder builder(this, current_.get());
    {
        ManifestReporter reporter(&s);
        log::Reader reader(file.get(), &reporter, /*checksum=*/true);
        std::string record;
        while (reader.read_record(&record) && s.is_ok()) {
            VersionEdit edit;
            s = edit.decode_from(record);
            if (s.is_ok() && edit.has_comparator() && edit.comparator() != icmp_->user_comparator()->name()) {
                s = Status::invalid_argument(edit.comparator() + " does not match the comparator in use",
                                             std::string(icmp_->user_comparator()->name()));
            }
            if (s.is_ok()) s = builder.apply(edit);
            if (edit.has_log_number()) {
                log_number = edit.log_number();
                have_log_number = true;
            }
            if (edit.has_next_file_number()) {
                next_file = edit.next_file_number();
                have_next_file = true;
            }
            if (edit.has_last_sequence()) {
                last_sequence = edit.last_sequence();
                have_last_sequence = true;
            }
        }
    }
    file.reset();

    if (s.is_ok()) {
        if (!have_next_file) {
            s = Status::corruption("MANIFEST has no next-file entry");
        } else if (!have_log_number) {
            s = Status::corruption("MANIFEST has no log-number entry");
        } else if (!have_last_sequence) {
            s = Status::corruption("MANIFEST has no last-sequence entry");
        }
    }
    if (!s.is_ok()) return s;

    auto v = std::make_shared<Version>(this);
    s = builder.save_to(v.get());
    if (!s.is_ok()) return s;
    finalize(v.get());
    append_version(std::move(v));

    // The MANIFEST written after recovery takes the next number.
    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    mark_file_number_used(log_number);
    return Status::ok();
}

void VersionSet::add_live_files(std::set<std::uint64_t>* live) {
    for (const auto& weak : versions_) {
        if (auto v = weak.lock()) {
            for (int level = 0; level < num_levels_; ++level) {
                for (const auto& f : v->files(level)) live->insert(f->number);
            }
        }
    }
}

std::uint64_t VersionSet::approximate_offset_of(const Version& v, const InternalKey& key) {
    std::uint64_t result = 0;
    for (int level = 0; level < num_levels_; ++level) {
        for (const auto& f : v.files(level)) {
            if (icmp_->compare(f->largest.encode(), key.encode()) <= 0) {
                result += f->file_size;  // the whole file is before key
            } else if (icmp_->compare(f->smallest.encode(), key.encode()) > 0) {
                // The whole file is after key. Deeper levels are sorted, so
                // every later file is after it too.
                if (level > 0) break;
            } else {
                std::shared_ptr<Table> table;
                if (table_cache_->find_table(f->number, f->file_size, &table).is_ok()) {
                    result += table->approximate_offset_of(key.encode());
                }
            }
        }
    }
    return result;
}

std::string VersionSet::level_summary() const {
    std::string r = "files[";
    for (int level = 0; level < num_levels_; ++level) r += " " + std::to_string(current_->num_files(level));
    r += " ]";
    return r;
}

std::uint64_t VersionSet::max_next_level_overlapping_bytes() {
    std::uint64_t result = 0;
    std::vector<FileMetaPtr> overlaps;
    for (int level = 1; level + 1 < num_levels_; ++level) {
        for (const auto& f : current_->files(level)) {
            current_->get_overlapping_inputs(level + 1, &f->smallest, &f->largest, &overlaps);
            result = std::max(result, total_file_size(overlaps));
        }
    }
    return result;
}

void VersionSet::get_range(const std::vector<FileMetaPtr>& inputs, InternalKey* smallest, InternalKey* largest) const {
    assert(!inputs.empty());
    smallest->clear();
    largest->clear();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& f = inputs[i];
        if (i == 0) {
            *smallest = f->smallest;
            *largest = f->largest;
        } else {
            if (icmp_->compare(f->smallest.encode(), smallest->encode()) < 0) *smallest = f->smallest;
            if (icmp_->compare(f->largest.encode(), largest->encode()) > 0) *largest = f->largest;
        }
    }
}

void VersionSet::get_range2(const std::vector<FileMetaPtr>& a, const std::vector<FileMetaPtr>& b, InternalKey* smallest,
                            InternalKey* largest) const {
    std::vector<FileMetaPtr> all = a;
    all.insert(all.end(), b.begin(), b.end());
    get_range(all, smallest, largest);
}

std::unique_ptr<Iterator> VersionSet::make_input_iterator(Compaction* c) {
    ReadOptions options;
    options.verify_checksums = true;
    options.fill_cache = false;

    std::vector<std::unique_ptr<Iterator>> iters;
    for (int which = 0; which < 2; ++which) {
        const auto& inputs = c->inputs_[which];
        if (inputs.empty()) continue;
        if (c->level() + which == 0) {
            for (const auto& f : inputs) iters.push_back(table_cache_->new_iterator(options, f->number, f->file_size));
        } else {
            iters.push_back(new_level_iterator(table_cache_, *icmp_, options, inputs));
        }
    }
    return new_merging_iterator(icmp_, std::move(iters));
}

std::unique_ptr<Compaction> VersionSet::pick_compaction() {
    const auto v = current_;
    if (v->compaction_score_ < 1) return nullptr;
    const int level = v->compaction_level_;
    assert(level >= 0 && level + 1 < num_levels_);

    std::unique_ptr<Compaction> c(new Compaction(*options_, level, num_levels_, max_file_size_for_level(level + 1)));

    // Start after the key where the last compaction of this level stopped.
    const std::string& pointer = compact_pointer_[static_cast<std::size_t>(level)];
    for (const auto& f : v->files(level)) {
        if (pointer.empty() || icmp_->compare(f->largest.encode(), pointer) > 0) {
            c->inputs_[0].push_back(f);
            break;
        }
    }
    if (c->inputs_[0].empty()) c->inputs_[0].push_back(v->files(level).front());  // wrap around

    c->input_version_ = v;
    if (level == 0) {
        // Level-0 files overlap: take every one that overlaps the first.
        InternalKey smallest, largest;
        get_range(c->inputs_[0], &smallest, &largest);
        v->get_overlapping_inputs(0, &smallest, &largest, &c->inputs_[0]);
        assert(!c->inputs_[0].empty());
    }
    setup_other_inputs(c.get());
    return c;
}

void VersionSet::setup_other_inputs(Compaction* c) {
    const int level = c->level();
    const Version& v = *c->input_version_;

    InternalKey smallest, largest;
    get_range(c->inputs_[0], &smallest, &largest);
    v.get_overlapping_inputs(level + 1, &smallest, &largest, &c->inputs_[1]);

    InternalKey all_start, all_limit;
    get_range2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

    // See whether more files in `level` can join without pulling in more
    // files from level + 1.
    if (!c->inputs_[1].empty()) {
        std::vector<FileMetaPtr> expanded0;
        v.get_overlapping_inputs(level, &all_start, &all_limit, &expanded0);
        const std::uint64_t inputs1_size = total_file_size(c->inputs_[1]);
        const std::uint64_t expanded0_size = total_file_size(expanded0);
        if (expanded0.size() > c->inputs_[0].size() &&
            inputs1_size + expanded0_size < expanded_compaction_byte_size_limit(*options_)) {
            InternalKey new_start, new_limit;
            get_range(expanded0, &new_start, &new_limit);
            std::vector<FileMetaPtr> expanded1;
            v.get_overlapping_inputs(level + 1, &new_start, &new_limit, &expanded1);
            if (expanded1.size() == c->inputs_[1].size()) {
                smallest = new_start;
                largest = new_limit;
                c->inputs_[0] = std::move(expanded0);
                c->inputs_[1] = std::move(expanded1);
                get_range2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
            }
        }
    }

    if (level + 2 < num_levels_) v.get_overlapping_inputs(level + 2, &all_start, &all_limit, &c->grandparents_);

    // Remember where this compaction ended, so the next one of this level
    // starts after it. Recorded in the edit, so it survives a restart.
    compact_pointer_[static_cast<std::size_t>(level)].assign(largest.encode());
    c->edit_.set_compact_pointer(level, largest);
}

std::unique_ptr<Compaction> VersionSet::compact_range(int level, const InternalKey* begin, const InternalKey* end) {
    assert(level + 1 < num_levels_);
    std::vector<FileMetaPtr> inputs;
    current_->get_overlapping_inputs(level, begin, end, &inputs);
    if (inputs.empty()) return nullptr;

    // Bound the work done in one compaction. Not for level 0: its files
    // overlap, and leaving one out could leave an older version of a key
    // above a newer one.
    if (level > 0) {
        const std::uint64_t limit = max_file_size_for_level(level);
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            total += inputs[i]->file_size;
            if (total >= limit) {
                inputs.resize(i + 1);
                break;
            }
        }
    }

    std::unique_ptr<Compaction> c(new Compaction(*options_, level, num_levels_, max_file_size_for_level(level + 1)));
    c->input_version_ = current_;
    c->inputs_[0] = std::move(inputs);
    setup_other_inputs(c.get());
    return c;
}

// ---------------------------------------------------------------------------
// Compaction

Compaction::Compaction(const Options& options, int level, int num_levels, std::uint64_t max_output_file_size)
    : level_(level),
      max_output_file_size_(max_output_file_size),
      max_grandparent_overlap_(max_grandparent_overlap_bytes(options)),
      level_ptrs_(static_cast<std::size_t>(num_levels), 0) {}

Compaction::~Compaction() = default;

bool Compaction::is_trivial_move() const {
    return inputs_[0].size() == 1 && inputs_[1].empty() && total_file_size(grandparents_) <= max_grandparent_overlap_;
}

void Compaction::add_input_deletions(VersionEdit* edit) const {
    for (int which = 0; which < 2; ++which) {
        for (const auto& f : inputs_[which]) edit->remove_file(level_ + which, f->number);
    }
}

bool Compaction::is_base_level_for_key(std::string_view user_key) {
    const Comparator* ucmp = input_version_->vset_->icmp_->user_comparator();
    for (int lvl = level_ + 2; lvl < input_version_->num_levels(); ++lvl) {
        const auto& files = input_version_->files(lvl);
        auto& ptr = level_ptrs_[static_cast<std::size_t>(lvl)];
        while (ptr < files.size()) {
            const auto& f = files[ptr];
            if (ucmp->compare(user_key, f->largest.user_key()) <= 0) {
                // We've advanced far enough.
                if (ucmp->compare(user_key, f->smallest.user_key()) >= 0) return false;  // key falls in this file
                break;
            }
            ++ptr;
        }
    }
    return true;
}

bool Compaction::should_stop_before(std::string_view internal_key) {
    const InternalKeyComparator& icmp = *input_version_->vset_->icmp_;
    while (grandparent_index_ < grandparents_.size() &&
           icmp.compare(internal_key, grandparents_[grandparent_index_]->largest.encode()) > 0) {
        if (seen_key_) overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
        ++grandparent_index_;
    }
    seen_key_ = true;
    if (overlapped_bytes_ > max_grandparent_overlap_) {
        overlapped_bytes_ = 0;
        return true;
    }
    return false;
}

std::uint64_t Compaction::input_bytes() const {
    return total_file_size(inputs_[0]) + total_file_size(inputs_[1]);
}

}  // namespace strata
