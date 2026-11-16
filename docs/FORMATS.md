# File formats

All integers are little-endian. A **varint** stores 7 bits per byte, low
bits first, with the high bit set on every byte but the last (at most 5
bytes for 32-bit values, 10 for 64-bit). A **length-prefixed** string is a
varint32 length followed by the bytes. CRCs are CRC-32C (Castagnoli),
stored **masked**: `((crc >> 13) | (crc << 19)) + 0x6b2f9e45`, so that a CRC
over data containing CRCs does not degenerate.

## Database directory

| Name | Contents |
|---|---|
| `CURRENT` | The name of the live MANIFEST and a newline, e.g. `MANIFEST-000012\n`. Replaced atomically through `NNNNNN.dbtmp`. |
| `LOCK` | Locked while the database is open. |
| `LOG`, `LOG.old` | Human-readable information log (this run, previous run). |
| `MANIFEST-NNNNNN` | Record log of version edits. |
| `NNNNNN.log` | Write-ahead log: record log of write batches. |
| `NNNNNN.sst` | Table file. |

File numbers come from one counter, so a log, a table and a MANIFEST never
share a number. Numbers are printed with at least six digits.

## Record log (`.log` and MANIFEST)

The file is a sequence of 32 KiB blocks. Each record is written as one or
more **fragments**:

```
+---------+---------+------+-------------+
| crc (4) | len (2) | type | payload     |
+---------+---------+------+-------------+
crc  = masked CRC-32C over (type byte, payload)
type = 1 FULL | 2 FIRST | 3 MIDDLE | 4 LAST
```

A fragment never crosses a block boundary. When fewer than 7 bytes remain in
a block they are zero-filled and the next fragment starts the next block. A
record that fits goes out as one FULL fragment; otherwise FIRST, any number
of MIDDLEs, then LAST. An empty record is a FULL fragment with no payload.

The reader:

- treats a fragment cut short by end-of-file, or a header cut short there,
  as a write that never finished: end of log, no error;
- on a CRC mismatch or an impossible length, drops the rest of the block
  and reports it;
- skips a zero-filled region (type 0, length 0) silently;
- reports and drops fragments that arrive out of order (a MIDDLE or LAST
  without a FIRST, a FIRST followed by another FIRST or a FULL).

## Write batch (one WAL record)

```
fixed64 sequence | fixed32 count | update*
update = 0x01 key:length-prefixed value:length-prefixed     (put)
       | 0x00 key:length-prefixed                            (delete)
```

The *i*-th update (from 0) is applied with sequence `sequence + i`.

## Version edit (one MANIFEST record)

A list of tagged fields:

| Tag | Field | Encoding |
|---|---|---|
| 1 | comparator name | length-prefixed |
| 2 | log number | varint64 |
| 3 | next file number | varint64 |
| 4 | last sequence | varint64 |
| 5 | compaction pointer | varint32 level, length-prefixed internal key |
| 6 | deleted file | varint32 level, varint64 file number |
| 7 | new file | varint32 level, varint64 number, varint64 size, length-prefixed smallest internal key, length-prefixed largest internal key |

The first record of every MANIFEST is a snapshot: the comparator name, the
compaction pointers and every live file as "new file" entries. Levels must
be below the database's `num_levels` (and below 64 in any case).

## Internal key

```
user_key | fixed64( sequence << 8 | type )
type: 0 = deletion, 1 = value
```

## Table file (`.sst`)

```
[data block 0][trailer]
[data block 1][trailer]
...
[filter block][trailer]        optional
[properties block][trailer]
[metaindex block][trailer]
[index block][trailer]
[footer]                       48 bytes
```

**Trailer** (5 bytes, after every block): 1 byte compression type
(0 = none, 1 = strata-lz), then fixed32 masked CRC over the stored block
bytes and the type byte. A **block handle** is varint64 offset, varint64
size (size excludes the trailer).

**Footer**: metaindex handle and index handle, zero-padded to 40 bytes, then
fixed64 magic `0x3156544b41525453` ("STRATKV1" in file order).

### Blocks

Data, index, metaindex and properties blocks share one format: sorted
entries with prefix compression, then a restart array.

```
entry*  restart[0..n-1] (fixed32 each)  n (fixed32)
entry = varint32 shared | varint32 unshared | varint32 value_length
        key[shared..] (unshared bytes) | value
```

`shared` is the length of the prefix the key has in common with the
previous key. Every `block_restart_interval` entries (16 by default; 1 in
index and meta blocks) an entry is stored with `shared = 0` and its offset
is added to the restart array, so a reader can binary-search restart points
and then scan forward.

- **Data blocks** hold the table's entries; keys are internal keys.
- **Index block**: one entry per data block. Its key is a separator `>=` every
  key in that block and `<` every key in the next (shortened where the
  comparator can); its value is the block's handle.
- **Metaindex block**: name → handle, for `filter.bloom` and `properties`.
- **Properties block**: name → decimal or text value. `table.*` entries are
  written by the table builder (`num-entries`, `num-data-blocks`,
  `raw-key-size`, `raw-value-size`, `data-size`, `index-size`,
  `filter-size`, `compressed-blocks`, `comparator`); the database adds
  `strata.num-deletions`, `strata.smallest-sequence` and
  `strata.largest-sequence`.

### Filter block

One bloom filter over the **user keys** of every entry in the table:

```
bit array (ceil(bits / 8) bytes) | k (1 byte)
bits = max(64, num_keys * bits_per_key), rounded up to a byte
```

For a key, `h = hash32(key, 0xbc9f1d34)` and `delta = rotr(h, 15)`; probe
*i* tests bit `(h + i * delta) mod bits` for *i* in `0..k-1`. `k` above 30
is reserved: such a filter matches everything. `hash32` is defined in
`src/util/hash.cpp` and must not change.

### strata-lz compression

```
varint32 uncompressed_length | sequence*
sequence = token | [literal length bytes] | literals
           | offset (fixed16, 1..65535) | [match length bytes]
token    = (literal_count << 4) | (match_length - 4)
```

A nibble of 15 means more length bytes follow: each adds its value and a
byte below 255 ends the run. The stream may stop right after the literals
of the last sequence, once `uncompressed_length` bytes have been produced.
A match copies `match_length` bytes starting `offset` bytes back in the
output and may overlap itself (a run). A block is stored compressed only if
that saves at least 1/8 of its size.
