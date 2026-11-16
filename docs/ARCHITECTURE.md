# Architecture

This document follows a key through strata: how it is written, where it
lives, how it is found again, and how the files that hold it are merged and
recovered. File-level byte layouts are in [FORMATS.md](FORMATS.md).

## Source map

| Directory | What it holds |
|---|---|
| `include/strata/` | The public API: `DB`, `Options`, `WriteBatch`, `Iterator`, `Env`, `Status`, `Comparator`, dump helpers. |
| `src/util/` | Integer coding, CRC-32C, hashing, the arena, bloom filters, the LRU cache, the LZ compressor, histograms, string helpers. |
| `src/env/` | The local file system `Env` (POSIX and Windows), `MemEnv` (in memory, with crash and error injection), the file logger. |
| `src/table/` | Blocks, table files (builder and reader), table properties, merging and two-level iterators. |
| `src/db/` | Internal keys, the skiplist and memtable, write batches, the write-ahead log, version edits and version sets, compaction, the table cache, the user iterator, `DBImpl`, dumps. |
| `tools/` | The `strata` CLI (`cli.cpp`), `strata_bench`, `strata_fuzz`. |
| `tests/` | The doctest suite; `tests/support` has shared helpers. |

## Keys and sequence numbers

Every write gets a 56-bit **sequence number**, increasing by one per update.
Internally an entry is keyed by an **internal key**:

```
user_key | fixed64( sequence << 8 | type )      type: 1 = value, 0 = deletion
```

`InternalKeyComparator` orders internal keys by user key (with the user's
`Comparator`), then by sequence *descending*. So for one user key the newest
entry comes first, and seeking to `(key, S)` lands on the newest entry with
sequence `<= S`. That one property gives snapshots for free: a read at
snapshot `S` is a seek to `(key, S)`.

A delete writes a **deletion marker** (tombstone) rather than removing
anything; compaction later drops both the marker and the data it hides.

## The write path

```
DB::put/remove/write
  └─ DBImpl::write
       ├─ join the writer queue; wait to be at the front
       ├─ make_room_for_write   (may switch memtables or stall)
       ├─ build_batch_group     (merge queued batches into one)
       ├─ log::Writer::add_record   ── WAL append (+ sync if requested)
       ├─ WriteBatchInternal::insert_into(mem_)
       └─ wake the writers whose batches were in the group
```

- **Group commit.** Writers queue on `writers_`. The writer at the front
  merges the batches behind it (up to 1 MiB; a sync write never rides with a
  non-sync leader) into one log record, so many concurrent small writes cost
  one log append and at most one `fsync`.
- **Lock-free apply.** The front writer appends to the log and inserts into
  the memtable *without* the DB mutex. This is safe because only the front
  writer touches the log and `mem_`, and the skiplist allows concurrent
  readers with one writer. `last_sequence` is published after the insert,
  so readers never see a half-applied group.
- **Backpressure** (`make_room_for_write`): with
  `level0_slowdown_trigger` level-0 files each write sleeps 1 ms once; with
  `level0_stop_trigger` files, or while the previous memtable is still being
  flushed, writers wait on `background_done_cv_`.
- **Memtable switch.** When `mem_` exceeds `write_buffer_size`, a new log
  file is created, `mem_` becomes `imm_` (immutable), and the background
  thread is asked to flush it.
- **Errors.** A failed log *sync* (or a failed background write) sets
  `bg_error_`; from then on every write fails with it, because the database
  can no longer promise that what readers saw will survive recovery.

## The read path

`DBImpl::get` takes references to `mem_`, `imm_` and the current `Version`
under the mutex, releases it, and looks in that order — newest data first:

1. the memtable (`MemTable::get`: one skiplist seek),
2. the immutable memtable, if a flush is in progress,
3. the table files (`Version::get`): every level-0 file whose range covers
   the key, newest first; then in each deeper level, at most one file, found
   by binary search on the files' largest keys.

For each candidate table, the table's **bloom filter** (over user keys) is
checked first; a negative answer skips the table without reading a data
block. Otherwise the index block is searched and one data block read —
through the **block cache**.

The first entry found for the user key decides: a value is returned, a
deletion marker means `NotFound`.

### Iterators

`DBImpl::new_iterator` builds a **merging iterator** over one child per
memtable, one per level-0 file and one *concatenating* iterator per deeper
level (a two-level iterator: an index over the level's files, opening each
table lazily). That yields every internal key in order.

`DBIter` (in `db_iter.cpp`) turns that into the user's view: for each user
key it shows only the newest entry visible at the iterator's sequence and
skips keys whose newest visible entry is a deletion. Moving forward it sits
on the visible entry; moving backward it has to read all of a key's entries
(they arrive oldest first) and keeps the newest in `saved_key_`/
`saved_value_`. The comments at the top of the class spell out the
invariants for each direction.

An iterator pins the memtables and the `Version` it started with, so table
files it may still read are not deleted under it.

## Versions and the MANIFEST

A `Version` is an immutable list of table files per level. Level 0 holds
memtable flushes, whose key ranges may overlap; in levels 1 and deeper the
files are sorted and disjoint.

Every change — a flush adds a file; a compaction removes its inputs and adds
its outputs — is a `VersionEdit`. `VersionSet::log_and_apply` applies the
edit to the current version to build a new one, appends the edit to the
**MANIFEST** (a record log) and syncs it, then installs the new version.
`CURRENT` names the live MANIFEST. On open, a new MANIFEST is started with a
snapshot of the recovered state, and `CURRENT` is switched to it atomically
(write a temp file, rename it).

Versions are reference-counted (`shared_ptr`); the `VersionSet` remembers
every version it made (`weak_ptr`) so that "live files" means files used by
any version someone still holds.

## Flushes

`compact_memtable` writes `imm_` to a table (`build_table`) without holding
the mutex, then records it with `log_and_apply`, setting the edit's log
number to the current log so older logs become obsolete.

A flushed table that overlaps nothing in level 0 may be placed deeper
(`pick_level_for_memtable_output`, up to `max_memtable_level`) as long as
it overlaps nothing on the way and would not overlap too much data two
levels down. This saves compactions for sequential loads.

## Compaction

One background thread (`background_thread_main`) does all flushing and
compaction, one job at a time, flushes first.

**Picking.** After every version change `VersionSet::finalize` scores each
level: level 0 by file count / `level0_compaction_trigger`, level *n* by
bytes / `max_bytes_for_level_base * level_size_multiplier^(n-1)`. The level
with the highest score >= 1 is compacted:

- start with the first file after that level's **compaction pointer** (where
  the last compaction of the level ended), so compactions sweep the key
  space round-robin;
- for level 0, add every overlapping level-0 file (repeatedly, as the range
  grows);
- add every file in level+1 that overlaps; then, if more level files fit
  without pulling in more level+1 files, add them too;
- note the **grandparents**: files in level+2 overlapping the range.

**Trivial move.** One input file, nothing overlapping in level+1, and not
too much grandparent overlap: the file is moved by editing the MANIFEST
only.

**Merging** (`do_compaction_work`) walks a merging iterator over the inputs
without the mutex and writes new tables to level+1:

- an entry is dropped if a newer entry for the same user key exists with a
  sequence at or below the oldest live snapshot (no reader can see it);
- a deletion marker is dropped if it is at or below the oldest snapshot and
  no level below the output holds the key (`is_base_level_for_key`), since
  there is nothing left for it to hide;
- an output file is ended when it reaches `target_file_size` or would
  overlap more than 10 target sizes of grandparent data — but **never
  between two entries of the same user key**, so later compactions can
  never move a newer version of a key below an older one;
- if a memtable fills up meanwhile, it is flushed (to level 0) before the
  merge continues, so writers are not stalled behind a long compaction.

Outputs are synced, opened once to check them, and installed with one
`log_and_apply` that also deletes the inputs. Files no live version uses
are then deleted (`remove_obsolete_files`).

**Manual compaction.** `DB::compact_range` flushes the memtable, then
compacts each level from 0 down to the deepest level holding data in the
range, one bounded chunk at a time.

## Recovery

`DB::open`:

1. takes the `LOCK` file, opens the info `LOG`;
2. creates a new database if needed (`MANIFEST-000001` + `CURRENT`);
3. replays the MANIFEST named by `CURRENT` into a `Version`;
4. checks that every table the version lists exists;
5. replays each write-ahead log numbered at or after the MANIFEST's log
   number into memtables, flushing them to level-0 tables as they fill;
6. starts a fresh log and a fresh MANIFEST, deletes obsolete files and
   starts the background thread.

A log record cut short at the end of a log is a write that never
completed: it is dropped silently. Damage elsewhere is skipped and noted in
the `LOG`, or — with `paranoid_checks` — fails the open.

Crash safety comes from ordering: a table is synced before the MANIFEST
edit that adds it; the MANIFEST is synced before `CURRENT` points at it;
log files are deleted only after the edit making them obsolete is durable.

## Threading summary

| Thread | Does | Holds the mutex while |
|---|---|---|
| Writers | queue, group commit, memtable insert | queueing and publishing, not during log I/O |
| Readers | `get`, iterators | grabbing references only |
| Background | flushes, compactions, deleting files | picking work and installing results, not during I/O |

`mutex_` guards the writer queue, `mem_`/`imm_` pointers, the version set,
snapshots, pending outputs and `bg_error_`. Condition variables:
`background_work_cv_` wakes the background thread; `background_done_cv_`
is signalled after each background job (writers stalled on memtable space,
`flush()`, manual compactions and tests wait on it).

## Caches

- **Table cache** (`TableCache`): open `Table` objects keyed by file number,
  up to `max_open_files - 10`.
- **Block cache** (`LruCache<BlockCacheKey, Block>`): uncompressed data
  blocks keyed by (table id, block offset), up to `block_cache_bytes`.
  Values are `shared_ptr`s, so an evicted block stays valid for iterators
  still using it.

Both caches are sharded by key hash with one mutex per shard.
