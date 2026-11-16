# strata

An embedded, persistent key-value store built on a log-structured merge tree,
in portable C++20.

strata keeps an ordered map from byte-string keys to byte-string values on
disk. Writes go to a write-ahead log and an in-memory skiplist; full
memtables are written out as sorted, immutable table files; a background
thread merges those files down a hierarchy of levels so reads stay fast and
deleted or overwritten data is reclaimed.

```cpp
#include "strata/strata.hpp"

strata::Options options;
options.create_if_missing = true;
std::unique_ptr<strata::DB> db;
strata::Status s = strata::DB::open(options, "/tmp/inventory", &db);

s = db->put({}, "sku:1042", "12 in stock");
std::string value;
s = db->get({}, "sku:1042", &value);           // "12 in stock"

strata::WriteBatch batch;                       // applied atomically
batch.remove("sku:1042");
batch.put("sku:1043", "3 in stock");
s = db->write({}, &batch);

auto it = db->new_iterator({});                 // ordered scans
for (it->seek("sku:"); it->valid(); it->next()) { /* ... */ }
```

See [`examples/basic_usage.cpp`](examples/basic_usage.cpp) for snapshots and
durable writes.

## Features

- **Ordered key-value API**: `put`, `get`, `remove`, atomic `WriteBatch`es,
  bidirectional iterators with `seek`, consistent snapshots.
- **Durability**: every write is logged before it is applied; `sync` writes
  survive a machine crash, unsynced writes survive a process crash. Recovery
  replays the log and tolerates a torn final record.
- **LSM storage engine**: skiplist memtable, leveled compaction with
  round-robin file selection, grandparent-overlap limits, trivial moves,
  and deletion markers dropped once nothing below can be hidden.
- **Table files**: prefix-compressed blocks with restart points, a bloom
  filter per table, a properties block, CRC-32C on every block, and an
  optional built-in LZ77 block compressor.
- **Group commit**: concurrent writers are batched into one log write.
- **Caching**: a sharded LRU block cache and an open-table cache.
- **Pluggable environment**: all file system access goes through `Env`.
  `new_mem_env()` gives an in-memory file system for tests, with hooks to
  simulate crashes (dropping unsynced data) and I/O errors.
- **Tools**: the `strata` CLI (get/put/scan/load/export/compact/check/dump
  and an interactive shell), `strata_bench`, and `strata_fuzz`.
- **Portable**: Linux, macOS and Windows (MSVC or MinGW); no dependencies
  beyond the standard library.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Or with the presets: `cmake --preset dev && cmake --build --preset dev &&
ctest --preset dev`. Presets `asan` and `tsan` build with AddressSanitizer +
UBSan and ThreadSanitizer.

On Windows, open the folder in Visual Studio (it reads `CMakeLists.txt`
directly) or run the same `cmake` commands from a Developer Command Prompt.

| CMake option | Default | |
|---|---|---|
| `STRATA_BUILD_TESTS` | top-level only | the doctest suite (`strata_tests`) |
| `STRATA_BUILD_TOOLS` | top-level only | `strata`, `strata_bench`, `strata_fuzz`, the example |
| `STRATA_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` |
| `STRATA_SANITIZE` | OFF | ASan + UBSan (GCC/Clang) |
| `STRATA_TSAN` | OFF | ThreadSanitizer (GCC/Clang) |

As a dependency: `add_subdirectory(strata)` and link `strata::strata`.

## The command-line tool

```text
$ strata put /tmp/demo fruit mango
$ strata put /tmp/demo veg okra
$ strata get /tmp/demo fruit
mango
$ strata scan /tmp/demo
fruit	mango
veg	okra
$ strata load /tmp/demo more.tsv            # key<TAB>value lines
$ strata scan /tmp/demo --prefix f --limit 10
$ strata compact /tmp/demo
compacted: files[ 0 1 0 0 0 0 0 ]
$ strata check /tmp/demo                    # verify every block checksum
1 table files, 2 entries checked, 0 problems
$ strata dump /tmp/demo/000017.sst          # tables, logs and MANIFESTs
table 000017.sst (562 B)
  comparator:        strata.InternalKeyComparator
  entries:           2
  ...
  'fruit' @ 1 : put => 'mango'
  'veg' @ 2 : put => 'okra'
$ strata shell /tmp/demo                    # interactive; try 'help'
```

Keys and values are printed with non-printable bytes escaped as `\xNN`;
`--escape` makes the tool read its arguments the same way, so binary keys
round-trip. `strata --help` lists everything.

## Benchmarks

`strata_bench` runs the usual workloads. On a 2-core cloud VM, Clang
release build, 200,000 entries with 16-byte keys and 100-byte values:

```text
fillseq        :       1.915 micros/op;   57.8 MB/s
fillrandom     :       3.354 micros/op;   33.0 MB/s
overwrite      :       5.673 micros/op;   19.5 MB/s
readrandom     :       9.410 micros/op;
readseq        :       0.476 micros/op;  232.2 MB/s
readreverse    :       0.786 micros/op;  140.7 MB/s
readmissing    :       1.398 micros/op;
seekrandom     :      18.678 micros/op;
readrandom     :       5.229 micros/op;   (after a full compaction)
```

These are a baseline, not a claim: the engine favors clarity over tuning,
and [ROADMAP.md](ROADMAP.md) lists the obvious speedups.

## Testing

`strata_tests` has about 180 test cases across the codec, table, log,
version, database, recovery and CLI layers, including:

- randomized model checking: long random sequences of writes, batches,
  reads, iterations, snapshots, flushes, compactions and reopens compared
  against a `std::map`, under several configurations (no compression, tiny
  files, reverse key order, no caches, real disk);
- crash tests that drop unsynced data at random points and require every
  synced write to survive;
- damage tests for logs, tables, CURRENT and MANIFEST files, and I/O-error
  injection during flushes and log writes;
- multi-threaded tests of group commit, snapshot consistency and iterators
  racing compactions (run under ThreadSanitizer in CI).

`strata_fuzz` mutates valid encodings (blocks, tables, logs, version edits,
write batches, LZ streams and whole database directories) and feeds them to
the decoders; `ctest` runs short fixed-seed passes and the nightly workflow
runs longer ones under ASan.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how reads, writes, flushes,
  compactions and recovery work, and the threading model.
- [docs/FORMATS.md](docs/FORMATS.md): byte-level formats of every file.
- [ROADMAP.md](ROADMAP.md): features not built yet.

## License

MIT. Copyright (c) 2026 Tropibyte, Inc. The vendored
[doctest](third_party/doctest) is MIT-licensed by Viktor Kirilov.
