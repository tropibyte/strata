# Roadmap

Features strata does not have yet. None of them is started anywhere in the
code. Each one names the files it touches and what "done" means, and each
needs new tests next to the existing ones in `tests/`.

## Features

### 1. Merge operator (read-modify-write)
Let callers register `Options::merge_operator` (e.g. counters, list append)
and call `DB::merge(key, operand)` without reading first.
- New value type `Merge` in `db/dbformat.hpp`, stored like a put.
- `MemTable::get` and `Version::get` collect operands newest-first until
  they reach a value, deletion or the bottom, then apply them.
- `DBIter` merges operands for each user key in both directions.
- Compaction combines operands when it can (`partial_merge`) and must keep
  them when a snapshot separates them.
- **Done when** a counter incremented by 4 threads with snapshots, flushes,
  compactions and reopens always reads the right total (extend
  `test_db_model.cpp` with merge operations).

### 2. Range deletion
`DB::delete_range(begin, end)` removes every key in `[begin, end)` in
O(1) writes.
- Range tombstones go in the memtable alongside point entries and into a
  new `range_del` meta block in each table.
- `get` and iterators must hide keys covered by a newer tombstone at the
  read's snapshot; compaction drops covered entries and drops tombstones at
  the bottom level.
- **Done when** the model test can mix `delete_range` with everything else.

### 3. Column families
Independent keyspaces (own memtable, levels and options) sharing one WAL
and MANIFEST: `DB::create_column_family`, `ColumnFamilyHandle` arguments on
every read and write, atomic batches across families. Touches `DBImpl`,
`VersionSet`, `VersionEdit` (a family id on every file), recovery.

### 4. Time-to-live
`Options::default_ttl` and `WriteOptions::ttl`: entries carry an expiry time
in the value encoding; reads hide expired entries and compaction drops
them. Needs an injectable clock (`Env::now_micros` is monotonic, not wall
time) so tests can control expiry.

### 5. Iterator bounds and prefix seek
`ReadOptions::iterate_lower_bound` / `iterate_upper_bound` and
`ReadOptions::prefix_same_as_start`. Iterators stop at the bound without
reading past it, and table iterators skip whole files and blocks outside
the bounds. Pair with an optional **prefix bloom filter** (`Options::prefix_extractor`)
so prefix scans skip tables that hold no key with the prefix.

### 6. Checkpoints and backups
`DB::checkpoint(dir)`: a consistent copy of the database another process can
open, made by hard-linking live tables (copying where links are not
possible) and copying the MANIFEST and current log up to a known sequence.
Must work while writes continue. Add `strata checkpoint <db> <dir>`.

### 7. Repair
`repair_db(path, options)` and `strata repair <db>`: rebuild a MANIFEST from
whatever table files and logs survive when the MANIFEST or CURRENT is lost
or damaged — scan every table for its key range and largest sequence,
convert logs to tables, and place everything in level 0 (or in disjoint
deeper levels when ranges allow). **Done when** deleting `CURRENT` and every
MANIFEST from a populated database and repairing it loses no synced write.

### 8. Optimistic transactions
`TransactionDB::begin()` returning a `Transaction` with `get`, `put`,
`remove`, `get_for_update` and `commit`. Commit validates that no key read
for update changed since the transaction's snapshot, then writes one batch.

### 9. Key-value separation for large values
Values above `Options::blob_threshold` go to append-only blob files; tables
store a (file, offset, size) pointer. Requires blob garbage collection
during compaction and blob-aware `get`, iterators, `check` and `dump`.

## Performance

### 10. Heap-based merging iterator
`table/merging_iterator.cpp` finds the next child with a linear scan of all
children, and changing direction re-seeks every child. Use a min-heap /
max-heap and keep it valid across direction changes. **Done when**
`strata_bench --benchmarks=seekrandom,readseq` with many level-0 files
improves and the merging-iterator tests still pass.

### 11. Multi-threaded and mixed benchmarks
`strata_bench --threads=N`, per-thread histograms merged at the end, and
YCSB-style workloads A–F (read/update ratios, Zipfian key choice). Report
p50/p99/p99.9 latency.

### 12. Parallel compaction
One background thread does all flushes and compactions. Allow
`Options::max_background_jobs`: flushes on their own thread, and several
compactions at once when their key ranges and levels do not conflict
(track files being compacted so two jobs never pick the same file).
Optionally split one large compaction into key-range subcompactions.

### 13. Positional reads on Windows and memory-mapped reads
On Windows, `RandomAccessFile` serializes reads with a mutex around
`_fseeki64`/`fread` (`src/env/env_default.cpp`). Use `ReadFile` with an
`OVERLAPPED` offset, which is thread-safe. Add an optional mmap-based
reader (`Options::use_mmap_reads`) on both platforms.

### 14. Cache-friendly and partitioned filters
Replace the whole-table bloom filter with a blocked bloom filter (each key's
probes within one 64-byte cache line), and for large tables partition the
filter per data block with a top-level index, so a lookup reads only one
filter partition. Keep reading tables written in the current format.

### 15. Seek-triggered compaction
Count lookups that had to read a file without finding their key there
(`allowed_seeks` per file, as a function of file size). A file that wastes
too many seeks becomes a compaction candidate even if its level is not too
big. Needs `Version::get` to report the first file it read in vain.

### 16. Compaction I/O rate limiting
`Options::rate_limit_bytes_per_sec`: a token-bucket limiter around
compaction and flush writes so background work does not starve foreground
reads. Show time spent waiting in `strata.stats`.

## Robustness and operations

### 17. Treat any write-ahead-log append failure as fatal
Today only a failed log *sync* stops further writes. A failed *append*
returns an error to that writer, but `log::Writer` has already advanced its
block offset, so later records can be laid out across block boundaries the
reader does not expect, and recovery may drop them. Fix by recording a
background error on any failed append (or by closing the log and rolling
to a new one), and add a test that fails one append, keeps writing, crashes
and checks what recovery returns.

### 18. MANIFEST rollover while running
A new MANIFEST is only started at open; a long-running database appends to
one MANIFEST forever. Start a new one (snapshot + switch `CURRENT`) when the
current one passes `Options::max_manifest_file_size`.

### 19. Statistics
An `Options::statistics` object with tickers (bytes written, block cache
hits per block type, bloom filter useful/useless, stall micros) and
histograms (get, write, compaction latency), exposed through
`strata.statistics` and `strata stats --json`.

### 20. Write-stall reporting and tuning
Record when and why writes stall (memtable full, too many level-0 files,
slowdown sleeps) with counts and total time, expose them as properties, and
add a soft limit on pending compaction bytes that triggers slowdowns
earlier.

## Known limitations

- Every snapshot and iterator must be released before the database is
  destroyed.
- `write_buffer_size` below about 8 KiB flushes after every write: the
  memtable's arena allocates 4 KiB blocks and counts whole blocks.
- One background thread does all flushes and compactions (see 12).
- On Windows, random reads of one table file are serialized (see 13).
- A failed log append does not stop further writes (see 17).
