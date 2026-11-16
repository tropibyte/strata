// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Versions and the version set.
//
// A Version is an immutable snapshot of which table files make up each
// level. Level 0 holds memtable flushes, whose key ranges may overlap; in
// every deeper level the files are sorted and disjoint. Readers and
// iterators hold a shared_ptr<Version>, so the files they use stay on disk
// until they let go.
//
// The VersionSet owns the current Version, the MANIFEST that records how it
// came to be, and the counters (next file number, last sequence). It also
// decides what to compact next.
//
// Thread safety: VersionSet methods require the DB mutex unless noted.
// Versions are immutable and can be read without it.

#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbformat.hpp"
#include "db/log_writer.hpp"
#include "db/version_edit.hpp"
#include "strata/iterator.hpp"
#include "strata/options.hpp"

namespace strata {

class Compaction;
class TableCache;
class VersionSet;

/// Index of the first file in `files` (sorted, disjoint) whose largest key
/// is >= `key`; files.size() if there is none.
std::size_t find_file(const InternalKeyComparator& icmp, const std::vector<FileMetaPtr>& files, std::string_view key);

/// True if some file in `files` overlaps the user-key range [*smallest,
/// *largest]; a null bound is unbounded on that side. `disjoint` says the
/// files are sorted and non-overlapping, allowing a binary search.
bool some_file_overlaps_range(const InternalKeyComparator& icmp, bool disjoint, const std::vector<FileMetaPtr>& files,
                              const std::string_view* smallest_user_key, const std::string_view* largest_user_key);

class Version {
public:
    explicit Version(VersionSet* vset);

    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    /// Point lookup in the table files. Returns NotFound if the newest
    /// visible entry is a deletion or there is none.
    Status get(const ReadOptions& options, const LookupKey& key, std::string* value) const;

    /// Appends iterators that together yield every entry in this version:
    /// one per level-0 file and one per deeper non-empty level.
    void add_iterators(const ReadOptions& options, std::vector<std::unique_ptr<Iterator>>* iters) const;

    /// True if a file in `level` overlaps the user-key range.
    bool overlap_in_level(int level, const std::string_view* smallest_user_key,
                          const std::string_view* largest_user_key) const;

    /// The level a new memtable flush covering [smallest, largest] should go
    /// to: 0, or deeper when nothing in the way overlaps it.
    int pick_level_for_memtable_output(std::string_view smallest_user_key, std::string_view largest_user_key) const;

    /// Files in `level` that overlap [begin, end] (null = unbounded). For
    /// level 0 the range grows to cover every file it pulls in, since level-0
    /// files may overlap each other.
    void get_overlapping_inputs(int level, const InternalKey* begin, const InternalKey* end,
                                std::vector<FileMetaPtr>* inputs) const;

    int num_levels() const noexcept { return static_cast<int>(files_.size()); }
    int num_files(int level) const { return static_cast<int>(files_[static_cast<std::size_t>(level)].size()); }
    const std::vector<FileMetaPtr>& files(int level) const { return files_[static_cast<std::size_t>(level)]; }
    std::uint64_t level_bytes(int level) const;

    double compaction_score() const noexcept { return compaction_score_; }
    int compaction_level() const noexcept { return compaction_level_; }

    /// One line per file, grouped by level.
    std::string debug_string() const;

private:
    friend class VersionSet;
    friend class Compaction;

    std::unique_ptr<Iterator> new_concatenating_iterator(const ReadOptions& options, int level) const;

    VersionSet* vset_;
    std::vector<std::vector<FileMetaPtr>> files_;

    // Set by VersionSet::finalize: the level most in need of compaction and
    // its score (>= 1 means it should be compacted).
    double compaction_score_ = -1;
    int compaction_level_ = -1;
};

class VersionSet {
public:
    VersionSet(std::string dbname, const Options* options, TableCache* table_cache, const InternalKeyComparator* icmp);
    ~VersionSet();

    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    /// Applies `edit` to the current version, records it in the MANIFEST
    /// (creating a new MANIFEST if none is open) and makes the result
    /// current. `lock` holds the DB mutex; it is released while writing.
    /// Only one call may be in progress at a time.
    Status log_and_apply(VersionEdit* edit, std::unique_lock<std::mutex>& lock);

    /// Reads CURRENT and replays the MANIFEST it names.
    Status recover();

    std::shared_ptr<Version> current() const { return current_; }

    std::uint64_t manifest_file_number() const noexcept { return manifest_file_number_; }
    std::uint64_t new_file_number() noexcept { return next_file_number_++; }

    /// Gives back a number from new_file_number() that went unused.
    void reuse_file_number(std::uint64_t number) noexcept {
        if (next_file_number_ == number + 1) next_file_number_ = number;
    }

    /// Ensures future file numbers are above `number`.
    void mark_file_number_used(std::uint64_t number) noexcept {
        if (next_file_number_ <= number) next_file_number_ = number + 1;
    }

    SequenceNumber last_sequence() const noexcept { return last_sequence_; }
    void set_last_sequence(SequenceNumber s) noexcept { last_sequence_ = s; }

    /// Write-ahead logs numbered below this are no longer needed.
    std::uint64_t log_number() const noexcept { return log_number_; }

    int num_levels() const noexcept { return num_levels_; }
    int num_level_files(int level) const { return current_->num_files(level); }
    std::uint64_t num_level_bytes(int level) const { return current_->level_bytes(level); }

    /// The next automatic compaction, or null if none is needed.
    std::unique_ptr<Compaction> pick_compaction();

    /// A compaction of the files in `level` overlapping [begin, end], or
    /// null if there are none.
    std::unique_ptr<Compaction> compact_range(int level, const InternalKey* begin, const InternalKey* end);

    bool needs_compaction() const { return current_->compaction_score_ >= 1; }

    /// Iterator over every input of the compaction, in key order. Does not
    /// need the DB mutex: it reads only the compaction's fixed inputs.
    std::unique_ptr<Iterator> make_input_iterator(Compaction* c);

    /// Adds the number of every file used by any live version.
    void add_live_files(std::set<std::uint64_t>* live);

    /// Approximate byte offset of `key` within the data of `v`.
    std::uint64_t approximate_offset_of(const Version& v, const InternalKey& key);

    /// Total size target for `level` (level >= 1).
    double max_bytes_for_level(int level) const;

    std::uint64_t max_file_size_for_level(int level) const;

    /// "files[ 2 5 1 0 0 0 0 ]"
    std::string level_summary() const;

    const InternalKeyComparator& icmp() const noexcept { return *icmp_; }
    TableCache* table_cache() const noexcept { return table_cache_; }
    const Options& options() const noexcept { return *options_; }

    /// Largest overlap in bytes between a file and the next level (for tests).
    std::uint64_t max_next_level_overlapping_bytes();

private:
    class Builder;
    friend class Compaction;
    friend class Version;

    void finalize(Version* v) const;
    void append_version(std::shared_ptr<Version> v);
    Status write_snapshot(log::Writer* log);

    void get_range(const std::vector<FileMetaPtr>& inputs, InternalKey* smallest, InternalKey* largest) const;
    void get_range2(const std::vector<FileMetaPtr>& a, const std::vector<FileMetaPtr>& b, InternalKey* smallest,
                    InternalKey* largest) const;
    void setup_other_inputs(Compaction* c);

    std::string dbname_;
    const Options* options_;
    TableCache* table_cache_;
    const InternalKeyComparator* icmp_;
    Env* env_;
    int num_levels_;

    std::uint64_t next_file_number_ = 2;
    std::uint64_t manifest_file_number_ = 0;
    SequenceNumber last_sequence_ = 0;
    std::uint64_t log_number_ = 0;

    std::unique_ptr<WritableFile> descriptor_file_;
    std::unique_ptr<log::Writer> descriptor_log_;

    std::shared_ptr<Version> current_;
    // Every version created, to find files still in use by old versions.
    std::list<std::weak_ptr<Version>> versions_;

    // Per level: the key where the next size compaction should start, so
    // compactions rotate through the key space.
    std::vector<std::string> compact_pointer_;
};

/// A compaction: inputs from `level` and `level + 1`, merged into new files
/// at `level + 1`.
class Compaction {
public:
    ~Compaction();

    int level() const noexcept { return level_; }

    /// The edit that will install this compaction's result.
    VersionEdit* edit() noexcept { return &edit_; }

    /// which: 0 for `level`, 1 for `level + 1`.
    int num_input_files(int which) const { return static_cast<int>(inputs_[which].size()); }
    const FileMetaPtr& input(int which, int i) const { return inputs_[which][static_cast<std::size_t>(i)]; }
    const std::vector<FileMetaPtr>& inputs(int which) const { return inputs_[which]; }

    std::uint64_t max_output_file_size() const noexcept { return max_output_file_size_; }

    /// A single file that can simply be moved down a level: nothing in the
    /// next level overlaps it, and it would not overlap too much of the level
    /// after that.
    bool is_trivial_move() const;

    /// Records the deletion of every input file in `edit`.
    void add_input_deletions(VersionEdit* edit) const;

    /// True if no level below the output level holds data for user_key, so
    /// a deletion marker for it can be dropped. Calls must come in key order.
    bool is_base_level_for_key(std::string_view user_key);

    /// True if the output file should end before `internal_key`, to keep it
    /// from overlapping too many files in the grandparent level. Calls must
    /// come in key order.
    bool should_stop_before(std::string_view internal_key);

    /// Lets go of the input version once the compaction is done with it.
    void release_inputs() { input_version_.reset(); }

    std::uint64_t input_bytes() const;

private:
    friend class VersionSet;

    Compaction(const Options& options, int level, int num_levels, std::uint64_t max_output_file_size);

    int level_;
    std::uint64_t max_output_file_size_;
    std::uint64_t max_grandparent_overlap_;
    std::shared_ptr<Version> input_version_;
    VersionEdit edit_;

    std::vector<FileMetaPtr> inputs_[2];
    std::vector<FileMetaPtr> grandparents_;  // level + 2 files overlapping the range
    std::size_t grandparent_index_ = 0;
    bool seen_key_ = false;
    std::uint64_t overlapped_bytes_ = 0;

    // For is_base_level_for_key: per level, a cursor into its files.
    std::vector<std::size_t> level_ptrs_;
};

}  // namespace strata
