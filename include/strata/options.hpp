// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Options for opening a database, reading and writing.

#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

class Comparator;
class Env;
class Logger;
class Snapshot;

/// How data blocks in table files are compressed.
enum class Compression : std::uint8_t {
    None = 0,
    /// strata's built-in LZ77 coder: fast, modest ratio.
    Lz = 1,
};

struct Options {
    // ---- Behavior on open ----------------------------------------------

    /// Defines the order of keys. Must match the one the database was created
    /// with. Default: bytewise_comparator().
    const Comparator* comparator = nullptr;

    /// File system and clock. Default: Env::default_env().
    Env* env = nullptr;

    /// Where informational messages go. Default: a LOG file in the database
    /// directory.
    Logger* info_log = nullptr;

    /// Create the database if it does not exist.
    bool create_if_missing = false;

    /// Fail if the database already exists.
    bool error_if_exists = false;

    /// Treat any damage found while recovering (a torn WAL record, a bad
    /// checksum) as an error instead of skipping the damaged data.
    bool paranoid_checks = false;

    // ---- Memtable and write path ---------------------------------------

    /// Bytes of updates the in-memory table holds before it is flushed to a
    /// level-0 table file. Larger means fewer, bigger files and a longer
    /// recovery after a crash.
    std::size_t write_buffer_size = 4u << 20;

    // ---- Table files ---------------------------------------------------

    /// Approximate uncompressed size of a data block.
    std::size_t block_size = 4096;

    /// Keys between restart points in a block (prefix compression resets at
    /// each one). Most people should leave this alone.
    int block_restart_interval = 16;

    /// Bloom filter bits per key in each table; 0 disables filters. 10 gives
    /// about a 1% false-positive rate.
    int bloom_bits_per_key = 10;

    Compression compression = Compression::Lz;

    /// Table files larger than this are split during compaction.
    std::size_t target_file_size = 2u << 20;

    // ---- Caches --------------------------------------------------------

    /// Number of open table files kept in the table cache.
    int max_open_files = 500;

    /// Bytes of uncompressed data blocks cached in memory; 0 disables the
    /// block cache.
    std::size_t block_cache_bytes = 8u << 20;

    // ---- Levels and compaction -----------------------------------------

    /// Number of levels, 2 to 20.
    int num_levels = 7;

    /// Number of level-0 files that triggers a compaction into level 1.
    int level0_compaction_trigger = 4;

    /// At this many level-0 files, each write is delayed by 1 ms to let
    /// compaction catch up.
    int level0_slowdown_trigger = 8;

    /// At this many level-0 files, writes wait until compaction reduces them.
    int level0_stop_trigger = 12;

    /// Target total size of level 1; each deeper level is
    /// level_size_multiplier times larger.
    std::uint64_t max_bytes_for_level_base = 10u << 20;
    double level_size_multiplier = 10.0;

    /// A flushed memtable that overlaps nothing may be placed directly in a
    /// level up to this one, skipping compactions. 0 always uses level 0.
    int max_memtable_level = 2;
};

struct ReadOptions {
    /// Verify block checksums on every read (they are always verified for
    /// compaction and in paranoid mode).
    bool verify_checksums = false;

    /// Put blocks read for this operation into the block cache. Turn off for
    /// bulk scans that would evict the working set.
    bool fill_cache = true;

    /// Read as of this snapshot instead of the latest state.
    const Snapshot* snapshot = nullptr;
};

struct WriteOptions {
    /// Flush the write-ahead log to stable storage before returning. Without
    /// it, a crash of the machine (not just the process) can lose the most
    /// recent writes, but never corrupts the database.
    bool sync = false;
};

}  // namespace strata
