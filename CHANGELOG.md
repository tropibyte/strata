# Changelog

## 0.1.0 — 2026-11-16

First release.

- Ordered key-value store with `put`, `get`, `remove`, atomic write batches,
  bidirectional iterators and snapshots.
- Write-ahead log with group commit, optional sync per write, and recovery
  that tolerates a torn final record (`paranoid_checks` makes damage fatal).
- Skiplist memtable; leveled compaction with round-robin file selection,
  grandparent-overlap limits, trivial moves and memtable push-down; manual
  `compact_range` and `flush`.
- Table files with prefix-compressed blocks, per-table bloom filters,
  properties, CRC-32C checksums and the built-in strata-lz compressor.
- Sharded LRU block cache and table cache.
- `Env` abstraction: POSIX and Windows file systems, and `MemEnv` with crash
  and I/O-error injection for tests.
- Properties: per-level file counts, compaction stats, file lists, memory
  usage, last sequence, background error.
- Tools: `strata` CLI (get/put/del/scan/load/export/compact/stats/property/
  size/check/destroy/dump/shell), `strata_bench`, `strata_fuzz`.
- Tests: about 180 doctest cases including randomized model checking, crash
  tests and concurrency tests; ASan/UBSan and TSan builds in CI.
