// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// DBImpl: the database.
//
// Write path: writers queue up; the writer at the front of the queue merges
// the batches behind it into one group, appends the group to the
// write-ahead log, inserts it into the memtable and wakes the others.
//
// When the memtable fills, it becomes the immutable memtable, a new log is
// started, and the background thread writes the immutable memtable to a
// level-0 table. The same thread runs compactions, which merge table files
// down the levels and drop entries no snapshot can see.
//
// Concurrency: one mutex (mutex_) guards the state below unless noted. The
// memtable, versions and tables are safe to read without it once a
// reference has been taken under it.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.hpp"
#include "db/log_writer.hpp"
#include "db/snapshot.hpp"
#include "strata/db.hpp"
#include "strata/env.hpp"
#include "table/table_options.hpp"

namespace strata {

class Compaction;
class MemTable;
class TableCache;
class TableFileWriter;
class Version;
class VersionEdit;
class VersionSet;
struct FileMetaData;

class DBImpl final : public DB {
public:
    DBImpl(const Options& options, std::string dbname);
    ~DBImpl() override;

    Status put(const WriteOptions& options, std::string_view key, std::string_view value) override;
    Status remove(const WriteOptions& options, std::string_view key) override;
    Status write(const WriteOptions& options, WriteBatch* batch) override;
    Status get(const ReadOptions& options, std::string_view key, std::string* value) override;
    std::unique_ptr<Iterator> new_iterator(const ReadOptions& options) override;
    const Snapshot* get_snapshot() override;
    void release_snapshot(const Snapshot* snapshot) override;
    bool get_property(std::string_view property, std::string* value) override;
    void get_approximate_sizes(std::span<const Range> ranges, std::span<std::uint64_t> sizes) override;
    Status compact_range(std::optional<std::string_view> begin, std::optional<std::string_view> end) override;
    Status flush() override;

    // ---- For tests -------------------------------------------------------

    /// Compacts the files in `level` overlapping [begin, end] into level+1.
    Status test_compact_range(int level, std::optional<std::string_view> begin, std::optional<std::string_view> end);

    /// Waits until background work is idle.
    Status test_wait_for_background();

    /// Number of files in each level, e.g. "2,1,0,0,0,0,0".
    std::string test_files_per_level();

    std::uint64_t test_max_next_level_overlapping_bytes();

private:
    friend class DB;
    struct Writer;
    struct CompactionState;

    struct ManualCompaction {
        int level = 0;
        bool done = false;
        std::optional<InternalKey> begin;  // nullopt: start of the key space
        std::optional<InternalKey> end;    // nullopt: end of the key space
    };

    struct LevelStats {
        std::uint64_t micros = 0;
        std::uint64_t bytes_read = 0;
        std::uint64_t bytes_written = 0;
        std::uint64_t count = 0;

        void add(const LevelStats& o) {
            micros += o.micros;
            bytes_read += o.bytes_read;
            bytes_written += o.bytes_written;
            count += o.count;
        }
    };

    using Lock = std::unique_lock<std::mutex>;

    // Opening.
    Status recover(VersionEdit* edit, bool* save_manifest, Lock& lock);
    Status recover_log_file(std::uint64_t log_number, VersionEdit* edit, SequenceNumber* max_sequence,
                            bool* save_manifest, Lock& lock);
    Status new_db();
    void open_info_log();

    // Writing.
    Status make_room_for_write(bool force, Lock& lock);
    WriteBatch* build_batch_group(Writer** last_writer);

    // Background work.
    void background_thread_main();
    void maybe_schedule_compaction();
    void background_compaction(Lock& lock);
    /// Flushes imm_. With allow_push_down false the table always goes to
    /// level 0 (used while a compaction is running, whose output range is
    /// not yet in the version).
    void compact_memtable(Lock& lock, bool allow_push_down = true);
    Status write_level0_table(MemTable* mem, VersionEdit* edit, const Version* base, Lock& lock);
    Status do_compaction_work(CompactionState* compact, Lock& lock);
    Status open_compaction_output_file(CompactionState* compact, Lock& lock);
    Status finish_compaction_output_file(CompactionState* compact);
    Status install_compaction_results(CompactionState* compact, Lock& lock);
    void cleanup_compaction(CompactionState* compact);
    void record_background_error(const Status& s);
    void remove_obsolete_files(Lock& lock);
    Status run_manual_compaction(int level, std::optional<std::string_view> begin, std::optional<std::string_view> end);

    SequenceNumber snapshot_sequence(const ReadOptions& options) const;

    // Constant after construction.
    Env* const env_;
    const InternalKeyComparator internal_comparator_;
    const Options options_;  // sanitized copy
    const std::string dbname_;
    TableOptions table_options_;
    std::unique_ptr<Logger> owned_info_log_;
    Logger* info_log_ = nullptr;

    std::unique_ptr<TableCache> table_cache_;
    std::unique_ptr<FileLock> db_lock_;

    std::mutex mutex_;
    std::atomic<bool> shutting_down_{false};
    std::condition_variable background_work_cv_;  // wakes the background thread
    std::condition_variable background_done_cv_;  // signalled after each background job
    std::shared_ptr<MemTable> mem_;
    std::shared_ptr<MemTable> imm_;  // being flushed
    std::atomic<bool> has_imm_{false};
    std::unique_ptr<WritableFile> logfile_;
    std::uint64_t logfile_number_ = 0;
    std::unique_ptr<log::Writer> log_;

    std::deque<Writer*> writers_;
    WriteBatch group_batch_;  // scratch for build_batch_group

    SnapshotList snapshots_;

    // Table files being written by a flush or compaction: not yet in any
    // version, but not obsolete either.
    std::set<std::uint64_t> pending_outputs_;

    bool background_scheduled_ = false;
    ManualCompaction* manual_compaction_ = nullptr;
    std::thread background_thread_;

    std::unique_ptr<VersionSet> versions_;

    // Set when a background write fails; from then on writes fail with it.
    Status bg_error_;

    std::vector<LevelStats> stats_;
};

/// Options with defaults filled in and out-of-range values clamped.
Options sanitize_options(const InternalKeyComparator* icmp, const Options& src);

}  // namespace strata
