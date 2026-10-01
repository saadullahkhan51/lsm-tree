# On-disk format

Everything the engine writes, byte by byte. Integers are little-endian.
`fixed32` and `fixed64` are 4 and 8 raw bytes. `varint32` and `varint64` are
base-128, least significant group first, with the high bit of each byte
meaning "more follows". A length-prefixed slice is a `varint32` length
followed by that many bytes.

## Files in a database directory

| Name | What it is |
|---|---|
| `NNNNNN.log` | Write-ahead log. One per memtable. |
| `NNNNNN.sst` | Sorted table. Immutable once written. |
| `MANIFEST-NNNNNN` | The log of version edits: which tables are in which level. |
| `CURRENT` | One line of text naming the live manifest, e.g. `MANIFEST-000005\n`. |
| `LOCK` | Held with `flock` while a process has the database open. |
| `NNNNNN.dbtmp` | A `CURRENT` being written, before it is renamed into place. |

File numbers come from one counter, so a log, a table, and a manifest never
share a number, and a higher number is always newer.

## Internal keys

Tables and memtables store internal keys:

```
internal_key := user_key | fixed64((sequence << 8) | type)
type         := 0 (deletion) | 1 (value)
```

Sequence numbers are 56 bits. Internal keys sort by user key ascending, then
by the 8-byte trailer descending, so the newest version of a user key comes
first and a seek to `(key, snapshot_sequence, 1)` lands on the newest version
that the snapshot can see.

## Checksums

Every checksum is CRC32C (Castagnoli), stored masked:

```
masked = ((crc >> 15) | (crc << 17)) + 0xa282ead8
```

Masking exists because computing the CRC of data that itself contains a CRC
has poor error-detection properties, and a log record can hold a table block.

## Log files (write-ahead log and manifest)

A log file is a sequence of 32,768-byte blocks. A block holds physical
records:

```
record := crc: fixed32   masked CRC32C of the type byte and the payload
          length: fixed16   payload length
          type: 1 byte      1 FULL, 2 FIRST, 3 MIDDLE, 4 LAST
          payload
```

A logical record that fits in what is left of the block is one FULL record.
Otherwise it is split into a FIRST fragment, zero or more MIDDLE fragments,
and a LAST fragment. A physical record never crosses a block boundary. If
fewer than 7 bytes remain in a block, they are zero-filled and skipped.

How the reader treats a record that is not well formed:

| What it finds | Where | Outcome |
|---|---|---|
| The file ends inside a header or a payload | end of file | Truncated tail. Not an error: the write was in progress at a crash. |
| A zero-filled header with only zeros after it | end of file | Truncated tail. Some filesystems extend a file before its data reaches disk, so power loss can leave zeros where unsynced appends were. |
| A zero-filled header with data after it | anywhere | Corruption. |
| A CRC mismatch, an unknown type, a length that runs past a full block | anywhere | Corruption. |
| A MIDDLE or LAST with no FIRST, or a FIRST or FULL inside an unfinished record | anywhere | Corruption. |

A final record whose length runs past the end of the file is ambiguous: the
write may have been cut off, or the length field may be damaged while a
shorter, complete record is present. The reader tells them apart by looking
for a shorter length that satisfies the stored CRC and is followed by
something a log could contain (the end of the file, a cut-off header, or a
header with a valid type). If it finds one, the length is damaged and the
record is corruption. A genuinely cut-off record passes that test only by a
CRC coincidence.

Recovery tolerates a truncated tail only in a log that no later log with
records follows. Older logs were synced in full when their memtable was
switched out, so a short record in one of them is corruption.

### Write-ahead log payload: a write batch

```
batch  := sequence: fixed64    sequence number of the first entry
          count: fixed32       number of entries
          entry*
entry  := 0x01 | key: length-prefixed | value: length-prefixed     (put)
        | 0x00 | key: length-prefixed                              (delete)
```

Entry `i` of a batch has sequence number `sequence + i`. Replay checks that
each batch starts exactly where the previous one ended; a gap is corruption.
One log record holds one group commit: the batches of every writer in the
group, concatenated into a single batch.

### Manifest payload: a version edit

A version edit is a sequence of tagged fields, each tag a `varint32`:

| Tag | Field | Encoding after the tag |
|---|---|---|
| 1 | comparator name | length-prefixed |
| 2 | log number | `varint64`. Logs with smaller numbers are no longer needed. |
| 3 | next file number | `varint64` |
| 4 | last sequence | `varint64` |
| 5 | compaction pointer | level `varint32`, internal key length-prefixed |
| 6 | deleted file | level `varint32`, file number `varint64` |
| 7 | new file | level `varint32`, file number `varint64`, file size `varint64`, smallest internal key and largest internal key, both length-prefixed |

An unknown tag, a level of 7 or more, or trailing bytes make the edit
corrupt. The first record of every manifest is a snapshot of the whole
version at the time the manifest was created; the rest are the edits since.
Replaying all of them yields the current version.

A new manifest becomes live by writing its name to a `.dbtmp` file, syncing
it, renaming it to `CURRENT`, and syncing the directory.

## Tables

```
table := data block* | filter block | metaindex block | index block | footer
```

Every block is followed by a 5-byte trailer:

```
trailer := compression type: 1 byte    0, no compression. Any other value is corruption.
           crc: fixed32                masked CRC32C of the block and the type byte
```

The checksum of a block is verified every time the block is read from the
file. There is no option to skip it.

### Data, index, and metaindex blocks

```
block   := entry* | restart: fixed32* | restart count: fixed32
entry   := shared: varint32        bytes of the key shared with the previous key
           non_shared: varint32    bytes of the key that follow
           value length: varint32
           key suffix (non_shared bytes) | value
```

Keys are prefix-compressed against the previous key. Every 16th entry
(`Options::block_restart_interval`) is a restart point: it stores its whole
key (`shared` is 0), and its offset goes into the restart array, which is
what a seek binary-searches.

- **Data block**: internal key to value. A block is closed once it reaches
  `Options::block_size` (4,096 bytes by default).
- **Index block**: one entry per data block. The key is a separator: at
  least the last key of that block and less than the first key of the next,
  shortened where possible. The value is the block's handle. Restart
  interval 1, so every entry can be binary-searched.
- **Metaindex block**: one entry, `filter.lsm.BloomFilter`, whose value is
  the filter block's handle. A reader that does not know a name ignores it.

A block handle is `offset: varint64 | size: varint64`, where the size does
not count the trailer.

### Filter block

One bloom filter over every user key in the table:

```
filter := bit array | k: 1 byte    the number of probes
```

There are `bits_per_key` bits per distinct key, at least 64 in total, rounded
up to whole bytes, and `k = floor(bits_per_key * 0.69)` clamped to 1..30. The
probes come from one 32-bit hash `h` by double hashing: probe `i` tests bit
`(h + i * delta) mod bits`, with `delta` being `h` rotated right by 17 bits.
A `k` above 30 is reserved, and a reader treats such a filter as matching
everything.

### Footer

The last 48 bytes of the file:

```
footer := metaindex handle | index handle | zero padding to 40 bytes
          magic: fixed64    0x454c4241544d534c, the bytes "LSMTABLE"
```

A reader rejects a file that is shorter than 48 bytes, whose magic does not
match, or whose handles point outside the file.
