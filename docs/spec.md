# LSM-Tree Storage Engine — Feature Map & Spec

An embedded, single-process, persistent ordered key-value store in C++20. LevelDB-shaped architecture, built from scratch, benchmarked honestly against LevelDB and RocksDB.

---

## 1. What this must be able to claim

The README is the deliverable. Every design decision below exists to make one of these claims true and provable:

1. **It survives a crash at any point.** Fault injection kills the process at a random point mid-workload, discards unsynced writes, and the database reopens with every acknowledged write intact. Run in a loop, thousands of iterations.
2. **It detects corruption rather than returning wrong data.** Flip any bit in any SSTable or log file and the read returns an error, never a silently wrong value.
3. **Snapshots are isolated from compaction.** A read at an old sequence number sees the old value even after the key has been overwritten, deleted, and compacted away.
4. **Compaction never drops a visible key.** An invariant checker runs after every compaction in debug builds.
5. **It is measured, not guessed.** Throughput, p99/p99.9 latency, write amplification, space amplification, and read amplification, side by side with LevelDB and RocksDB on identical hardware, with an analysis of where it loses and why.
6. **It is clean under sanitizers.** ASan, UBSan, and TSan in CI, with TSan specifically exercising the concurrent memtable and the compaction thread.

Claim 1 and claim 3 are the two that most hobby LSM implementations cannot make. They are the point of the project.

---

## 2. Explicit non-goals

Do not build these. A plan that includes them is wrong.

- Transactions beyond atomic `WriteBatch`. No two-phase commit, no pessimistic or optimistic transaction layer, no serializable isolation.
- Column families. Single keyspace.
- Secondary indexes, merge operators, TTL, column-level anything.
- Universal, FIFO, or tiered compaction. **Leveled only.** (Discuss the tradeoff in the README; do not implement a second strategy.)
- Distributed anything. No replication, no consensus, no networking. This is an embedded library.
- A SQL or query layer.
- Backup, checkpointing, or incremental restore.
- Plugin architecture, configurable comparator registry, custom Env implementations beyond the two required (posix + fault injection).
- Windows support. Linux only, with macOS as best-effort.
- Language bindings. Possibly a C API as a stretch, nothing else.
- Range deletions (`DeleteRange`). Tombstone handling for point deletes is already the subtlest part of the build; range tombstones double it.

---

## 3. Stack and conventions

| Item | Choice |
| --- | --- |
| Language | C++20. Concepts and `std::span` where they help; no coroutines. |
| Build | CMake + Ninja |
| Testing | GoogleTest + custom crash harness |
| Fuzzing | libFuzzer (block parser, log reader, manifest reader) |
| Benchmark | Custom `db_bench` clone + Google Benchmark for microbenchmarks |
| Checksums | crc32c (Castagnoli) via SSE4.2 `_mm_crc32_u64`, with a portable fallback |
| Compression | LZ4, optional at block level (see open decisions) |
| CI | GitHub Actions: build matrix (gcc/clang, Debug/Release/ASan/TSan/UBSan), test, fuzz smoke run |

No exceptions for control flow — a `Status` type returned everywhere, `[[nodiscard]]`. No `std::shared_ptr` in hot paths; intrusive refcounts for `Version` and `Block`.

**Build the `Env` abstraction on day one.** Every file operation goes through it: `NewWritableFile`, `NewRandomAccessFile`, `NewSequentialFile`, `RenameFile`, `SyncDir`. Retrofitting this later to add fault injection is painful and you will not do it.

---

## 4. Repo layout

```
/include/lsm/        Public headers: db.h, options.h, status.h, slice.h, iterator.h
/src
  /util              Arena, crc32c, coding (varint/fixed), hash, LRU cache
  /port              Env interface, PosixEnv
  /db                MemTable, SkipList, WriteBatch, LogWriter/LogReader,
                     VersionSet, VersionEdit, Compaction, DBImpl, DBIter
  /table             BlockBuilder, Block, FilterBlock, TableBuilder, Table,
                     TwoLevelIterator, MergingIterator
/test                Unit tests, crash harness, invariant checkers
/fuzz                libFuzzer targets
/bench               db_bench clone, comparison scripts, result plots
/docs                Format specs, design notes, benchmark writeups
```

---

## 5. Core data representation

### 5.1 Internal key encoding

Every user key is stored with an 8-byte trailer:

```
internal_key = user_key || ((sequence << 8) | value_type)   // little-endian uint64
```

- `sequence` is 56 bits, monotonically increasing per write entry.
- `value_type`: `kTypeDeletion = 0`, `kTypeValue = 1`.

**Comparator ordering:** user key ascending by the user comparator, then the trailer **descending** as a uint64. This puts the newest version of a key first, and since `kTypeValue > kTypeDeletion`, a value sorts before a tombstone at the same sequence. Seeks construct a lookup key with `kTypeValue` so they land on the newest visible version.

Getting this ordering right is foundational — every iterator, every compaction merge, and every snapshot read depends on it. Write exhaustive comparator tests before anything else is built on top.

### 5.2 Sequence numbers

- `VersionSet::last_sequence_` is the global counter, persisted in the manifest.
- A `WriteBatch` is assigned a starting sequence; each entry in the batch gets `start + i`.
- A snapshot is a pinned sequence number held in an intrusive doubly-linked list on `DBImpl`.
- A read at sequence `s` sees the newest version of each key with `seq <= s`.

---

## 6. Write path

### 6.1 WriteBatch

Contiguous byte buffer:

```
[8-byte sequence][4-byte count][record]*
record := kTypeValue  key_len key value_len value
        | kTypeDeletion key_len key
```

Lengths are varint. The batch is the unit of atomicity: it goes into the WAL as one record and into the memtable as one loop. Either all of it is recovered or none of it.

### 6.2 Group commit

This is the main concurrency showcase. Do not skip it.

- Writers enqueue themselves on a `std::deque<Writer*>` under the DB mutex.
- The writer at the head becomes the **leader**. It drains followers from the queue, concatenates their batches (up to a size cap, ~1MB), releases the mutex, and performs **one** WAL append and **one** fsync for the whole group.
- It then applies all entries to the memtable, reacquires the mutex, assigns sequence numbers, signals every follower, and wakes the new head.
- Followers do no I/O and no memtable work.

**Benchmark to produce:** throughput vs. thread count (1 → 64), with and without group commit, with `sync=true`. The gap is large and the chart is the README's best single image.

### 6.3 Write-ahead log format

Use LevelDB's record format — it is designed so a reader can resynchronize after a torn write, which is exactly what crash recovery needs.

- File is a sequence of **32 KiB blocks**.
- Each record: `[crc32c: 4][length: 2][type: 1][payload]`.
- `type` ∈ `{FULL=1, FIRST=2, MIDDLE=3, LAST=4}`. A logical record larger than the remaining block space is fragmented across blocks.
- Trailing space < 7 bytes in a block is zero-filled and skipped.
- The checksum covers the type byte and payload.

The reader must distinguish and report: truncated final record (normal after a crash — stop cleanly), bad CRC (corruption — report), and unexpected fragment type (corruption — report). A truncated tail is *not* an error; a bad CRC in the middle *is*. Encode that distinction explicitly.

### 6.4 Durability policy

- `WriteOptions::sync = true` → fsync the WAL before acknowledging. Nothing acknowledged is ever lost.
- `sync = false` → acknowledged writes may be lost on power failure but **never** on process crash, and the database must still open cleanly. State this contract precisely in the README; it is the single most misunderstood thing about LSM durability.
- On `fdatasync` vs `fsync`: use `fdatasync` for appends to a file whose size change is already durable; measure the difference and note it.

---

## 7. MemTable

- **Skiplist**, max height 12, branching factor 4, backed by an **arena** (bump allocator, 4 KiB chunks).
- Concurrency model: **one writer at a time (enforced by the group-commit leader), many concurrent readers, no locks on the read path.** Next-pointers are `std::atomic<Node*>` with release stores on insert and acquire loads on traverse. Readers may see a partially linked tower; the invariant is that level-0 links are always consistent and a reader never follows a dangling pointer.
- Arena memory is never freed until the whole memtable is dropped, which is what makes lock-free reads safe.
- Memtable size tracked by arena usage, not entry count. Flush at `write_buffer_size` (default 4 MiB for tests, 64 MiB for benchmarks).
- On flush trigger: the active memtable becomes immutable, a new memtable and a new WAL file are created, and a background flush is scheduled. At most one immutable memtable pending in v1 — writes stall if a second flush is needed before the first completes.

**TSan test:** N reader threads doing `Get` while the writer inserts, for 30 seconds, under TSan. Must be clean.

---

## 8. SSTable format

```
[data block 0]
[data block 1]
...
[filter block]
[metaindex block]      -> maps "filter.<policy_name>" to the filter block handle
[index block]          -> one entry per data block: separator key -> BlockHandle
[footer: 48 bytes]     -> metaindex handle, index handle, padding, 8-byte magic
```

A `BlockHandle` is `varint64 offset, varint64 size`.

### 8.1 Data block

- Keys are **prefix-compressed** against the previous key: `[shared_len][unshared_len][value_len][unshared_key_bytes][value_bytes]`, all lengths varint.
- A **restart point** every 16 keys stores a full key (shared_len = 0), enabling binary search.
- Block tail: `[restart_offset]* [num_restarts: 4]`.
- Block trailer (outside the block proper): `[compression_type: 1][crc32c: 4]`, where the CRC covers the block data and the type byte.

The index block's separator key is computed as the **shortest string** that is >= the last key of block N and < the first key of block N+1. Same trick for the final key. This meaningfully shrinks the index and is a small, pleasing detail to mention.

### 8.2 Bloom filter block

- Per-SSTable filter, default **10 bits per key** (~1% false positive rate), k = 6 hashes.
- Use double hashing (Kirsch–Mitzenmacher): derive all k probes from one 32-bit hash plus a rotation, rather than computing k independent hashes.
- Filter is built over **user keys**, not internal keys.

**Test to write:** insert 1M keys, probe 1M absent keys, report measured FPR against theoretical for bits/key ∈ {4, 6, 8, 10, 12, 16}. The table goes in the README.

### 8.3 Caches

- **Block cache**: sharded LRU, 16 shards by hash of the key to cut mutex contention. Key is `(file_number, block_offset)`. Default 8 MiB.
- **Table cache**: LRU of open file descriptors plus parsed index and filter blocks, bounded by `max_open_files`.
- Both built on one generic `ShardedLRUCache` with intrusive refcounting and a `Handle` type the caller must release.

---

## 9. Versions and the manifest

This is the subsystem that separates a real engine from a toy. Most hobby implementations keep a mutable list of SSTables in memory and lose it on crash.

### 9.1 Structures

- **`VersionEdit`** — a serializable delta: files added (level, file number, size, smallest key, largest key), files deleted (level, file number), log number, next file number, last sequence, compaction pointers.
- **`Version`** — an immutable snapshot of `vector<FileMetaData*> files_[7]`. Reference-counted. A reader pins the current Version for the duration of its read; compaction installs a new Version without disturbing in-flight readers.
- **`VersionSet`** — holds the current Version, the Version list, the manifest log writer, and the global counters.

### 9.2 Manifest

The manifest is itself a log file (same record format as the WAL) containing a sequence of `VersionEdit` records. On open: read `CURRENT`, open the named manifest, replay every edit onto an empty Version to reconstruct the current state.

**Atomic manifest rotation** — write the new manifest to a temp name, fsync it, write `CURRENT.tmp` containing the new manifest's filename, fsync it, `rename("CURRENT.tmp", "CURRENT")`, then **fsync the containing directory**.

That last fsync is the step nearly every implementation forgets. Without it, `rename` is not durable and a power failure can leave you pointing at a manifest that no longer exists. Call this out explicitly in the README — it is the kind of detail that signals you have actually thought about crash consistency rather than read a blog post about it.

### 9.3 Level invariants

- **L0**: files may overlap. Ordered by file number (newest first) for reads. Max 4 files before compaction triggers.
- **L1–L6**: files within a level are non-overlapping and sorted by key. Binary search to find the single candidate file for any key.
- Level size targets: L1 = 10 MiB, each level 10× the previous.
- Target SSTable size: 2 MiB.

Write a `Version::AssertInvariants()` that verifies non-overlap at L1+ and key-range correctness, and call it after every version install in debug builds.

---

## 10. Read path

`Get(key, snapshot)`:

1. Pin current Version, read `last_sequence` (or the snapshot's sequence).
2. Probe the mutable memtable, then the immutable memtable.
3. For L0: probe every file whose key range contains the key, newest file number first.
4. For L1+: binary search the level for the one file whose range contains the key; probe it.
5. For each file probed: consult the bloom filter first, then the index block, then the data block (via block cache).
6. First match wins — whether it is a value or a tombstone. A tombstone means "not found."

**Read amplification metric:** count files actually opened per `Get`. Report the distribution. It is the number that explains LSM read performance and almost nobody measures it.

---

## 11. Iterators

- **`Block::Iter`** — binary search over restart points, then linear scan with prefix reconstruction.
- **`TwoLevelIterator`** — index block iterator driving data block iterators; also reused for level concatenation.
- **`MergingIterator`** — binary heap over child iterators: memtable, immutable memtable, one iterator per L0 file, one concatenating iterator per L1+ level. Supports forward and backward.
- **`DBIter`** — wraps the merging iterator, applies snapshot visibility, skips shadowed versions and tombstones, exposes user keys.

Backward iteration in `DBIter` is genuinely tricky (you must scan backward past all versions of a key and then determine the visible one). Budget time for it or put it on the cut list honestly.

**Test:** a model-based test. Maintain a `std::map` alongside the DB, apply a random operation sequence (put, delete, flush, compact, snapshot, iterate), and assert full-scan equivalence after each step.

---

## 12. Compaction

### 12.1 Picker

Score every level: L0 by `file_count / 4`, L1+ by `total_bytes / max_bytes_for_level`. Pick the highest-scoring level above 1.0. Within a level, round-robin using a persisted per-level compaction pointer so compaction work spreads across the keyspace.

### 12.2 Input selection

1. Pick one file from level L (at the compaction pointer).
2. For L0, expand to include all other L0 files overlapping that range (they overlap by definition).
3. Find all files in L+1 overlapping the resulting range.
4. **Expand backward**: if adding more L+1 files doesn't grow the L+1 input set, pull in more L files for free. (LevelDB's `GetRange2` optimization — cheap and worth doing.)
5. **Grandparent limit**: stop writing the current output file early if the keys written so far overlap more than `10 × target_file_size` of data in level L+2. This prevents producing a file whose future compaction is enormous.

### 12.3 Merge and output

Merge inputs with a `MergingIterator`. For each internal key in sorted order, decide drop-or-keep:

```
if user_key != last_user_key:
    last_sequence_for_key = kMaxSequence     // first (newest) version of this key
drop = false

if last_sequence_for_key <= smallest_snapshot:
    drop = true                              // hidden by a newer version visible to all snapshots
elif type == kTypeDeletion
     and sequence <= smallest_snapshot
     and not base_version.KeyMayExistInLevelsAbove(level + 2, user_key):
    drop = true                              // tombstone with nothing left to hide

last_sequence_for_key = sequence
```

**A tombstone may only be dropped when it is provably shadowing nothing.** That requires both conditions: no snapshot can still see the deleted value, *and* no deeper level holds an older version of the key that would resurrect. Getting this wrong produces the classic LSM bug where deleted data comes back to life after compaction. Write a test specifically for resurrection.

Outputs roll to a new file at 2 MiB or at the grandparent limit, whichever comes first. The compaction is installed as a single `VersionEdit` — all inputs deleted, all outputs added — written to the manifest in one record.

### 12.4 Scheduling and write stalls

Single background thread in v1. Must implement backpressure or the benchmarks are meaningless:

- L0 files >= 8: sleep 1 ms per write (gentle slowdown).
- L0 files >= 12: block writes until compaction catches up.
- Immutable memtable still pending flush: block writes.

Report stall time as a benchmark metric. "Throughput with stalls excluded" is a lie and a reviewer who knows LSMs will spot it.

### 12.5 Obsolete file deletion

After installing a version, compute the live file set across all retained versions and unlink everything else in the directory. A file still open by an in-flight iterator must not be deleted — hence Version refcounting. Test it: start a long iterator, run heavy compaction, confirm the iterator completes correctly and no file is deleted out from under it.

---

## 13. Crash and corruption testing

This is the week that makes the project credible. Everything above is table stakes.

### 13.1 FaultInjectionEnv

An `Env` wrapper that:

- Tracks per-file which byte ranges have been fsync'd.
- On `SimulateCrash()`, drops all unsynced buffered data, and optionally **truncates the last write to a random prefix** to simulate a torn write.
- Can reorder unsynced writes across files (filesystems do not guarantee ordering between files without fsync).
- Can inject `EIO` on any operation with configurable probability.
- Can refuse `SyncDir` silently, to prove the directory-fsync requirement matters — run a test with it disabled and show the manifest recovery failure it causes.

### 13.2 Crash loop

```
for iteration in 1..N:
    open db with FaultInjectionEnv (seeded RNG)
    run random workload, recording every acknowledged write
    crash at a random point
    reopen
    assert: every write acknowledged with sync=true is present
    assert: db state is a valid prefix of the operation history
    assert: no invariant violations in the recovered Version
```

Run it in CI with a fixed seed set, and nightly with random seeds. Report the iteration count in the README — "survived 50,000 randomized crash-recovery cycles" is a concrete, checkable claim.

### 13.3 Corruption

- Bit-flip harness: for each SSTable and log file, flip one bit at a random offset, reopen, read every key. Every outcome must be either correct data or an explicit corruption `Status`. Never wrong data returned as success.
- libFuzzer targets on: block decoder, WAL record reader, manifest/VersionEdit decoder, footer parser. These parse untrusted bytes after a crash and are the natural fuzz surface.

### 13.4 Concurrency

- TSan: readers + writer on the memtable; readers + compaction thread on the Version list; iterator lifetime vs. file deletion.
- A stress test: 16 threads doing mixed put/get/delete/iterate for 10 minutes against a model map, with compaction forced aggressively via a tiny `write_buffer_size`.

---

## 14. Benchmarks

Build a `db_bench` clone with the standard workloads so numbers are directly comparable to published LevelDB and RocksDB results:

`fillseq`, `fillrandom`, `overwrite`, `fillsync`, `readrandom`, `readseq`, `readreverse`, `readmissing`, `seekrandom`, `readwhilewriting`.

Then a YCSB-style mix: A (50/50 read-update), B (95/5), C (read-only), D (read-latest), E (short scans), F (read-modify-write).

**Report for each:** ops/sec, p50 / p99 / p99.9 latency, bytes written to disk, bytes read from disk.

**Derived metrics that matter most:**

- **Write amplification** = total bytes written to disk / total bytes written by the user. Chart it against level size multiplier (4, 8, 10, 16) — this single chart proves you understand the structure's central tradeoff.
- **Space amplification** = on-disk size / live data size, sampled over time.
- **Read amplification** = files opened per `Get`, distribution.

**Comparison protocol:** same machine, same value sizes (16-byte keys, 100-byte values plus a large-value run), same block cache size, same compression setting, three runs, report median. Document the hardware, the filesystem, and whether the page cache was dropped between runs.

**Realistic targets:**

- `fillrandom`: within 2× of RocksDB.
- `readrandom` with warm cache: within 2× of RocksDB.
- `readrandom` cold: you will lose badly. RocksDB has partitioned indexes, better bloom placement, and prefetching. Say so, quantify it, and describe what you would build next.

Publishing a loss with a correct explanation reads far better than a suspicious win. Lead with the analysis.

---

## 15. Public API

```cpp
struct Options {
  size_t write_buffer_size = 4 << 20;
  int max_open_files = 1000;
  size_t block_size = 4096;
  int block_restart_interval = 16;
  size_t block_cache_size = 8 << 20;
  int bloom_bits_per_key = 10;
  CompressionType compression = kNoCompression;
  bool paranoid_checks = false;
  Comparator* comparator = BytewiseComparator();
  Env* env = Env::Default();
};

class DB {
 public:
  static Status Open(const Options&, const std::string& path, DB** out);
  Status Put(const WriteOptions&, const Slice& key, const Slice& value);
  Status Delete(const WriteOptions&, const Slice& key);
  Status Write(const WriteOptions&, WriteBatch*);
  Status Get(const ReadOptions&, const Slice& key, std::string* value);
  Iterator* NewIterator(const ReadOptions&);
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot*);
  bool GetProperty(const Slice& name, std::string* value);
  Status CompactRange(const Slice* begin, const Slice* end);
};
```

Keep it LevelDB-compatible in shape. It costs nothing and it means a reviewer can read your API in five seconds.

`GetProperty` should expose level file counts, level sizes, cumulative write amplification, stall time, and cache hit rates — the benchmark harness reads these rather than reimplementing accounting.

---

## 16. Build order

**Phase 0 — Foundations (2–3 days).** CMake, `Slice`, `Status`, `Comparator`, `Arena`, varint/fixed coding, crc32c with hardware path, `ShardedLRUCache`, `Env` + `PosixEnv`. Exhaustive unit tests on coding and the comparator. Nothing here is interesting but everything depends on it.

**Phase 1 — Durable memtable (3–4 days).** Skiplist, `WriteBatch`, WAL writer/reader with the 32 KiB block format, group commit, recovery by replay. At the end of this phase you have a working crash-safe KV store with no persistence beyond the log. First crash test here, not later.

**Phase 2 — SSTables (4–5 days).** `BlockBuilder`/`Block`, prefix compression, restart points, bloom filter block, `TableBuilder`/`Table`, footer, table cache, block cache. Memtable flush to a single L0 file. Get path reads memtable then L0.

**Phase 3 — Versions (3 days).** `VersionEdit`, `Version`, `VersionSet`, manifest log, `CURRENT` with atomic rotation and directory fsync, recovery from manifest, level structure, binary-search reads at L1+. Invariant checker.

**Phase 4 — Compaction (4–5 days).** Picker, input selection with expansion and grandparent limit, merge with the drop rules, output rolling, atomic install, obsolete file deletion, write stalls, background thread. Resurrection test.

**Phase 5 — Snapshots and iterators (3 days).** Snapshot list, `DBIter` with visibility rules, `MergingIterator`, forward and backward scan, the model-based equivalence test.

**Phase 6 — Fault injection (3–4 days).** `FaultInjectionEnv`, crash loop harness, bit-flip harness, fuzz targets, sanitizer CI.

**Phase 7 — Measurement and writing (3 days).** `db_bench`, YCSB mix, comparison runs against LevelDB and RocksDB, plots, format documentation in `/docs`, README, and one long build-log post about the hardest bug.

If time runs out, cut in this order: backward iteration → compression → YCSB mix (keep `db_bench`) → multi-level compaction beyond L3. **Never cut Phase 6.** An LSM without crash testing is a sorted-file exercise.

---

## 17. Documentation deliverables

- `README.md` — what it is, a benchmark table in the first screen, architecture diagram, the six claims from §1 each linked to the test that proves it.
- `docs/format.md` — exact byte layouts for the WAL record, data block, filter block, index block, footer, and manifest. Writing this down catches bugs.
- `docs/durability.md` — the precise durability contract, what fsync guarantees, why the directory fsync is required, what is lost on power failure vs process crash.
- One build-log post on a genuinely hard bug. Strong candidates: the tombstone resurrection case, a torn-write recovery edge case, or a TSan race in the skiplist. This doubles as your writing sample.

---

## 18. Decisions left open for the planner

Give a recommendation with reasoning on each; do not pick silently.

1. **Memtable structure** — skiplist vs. a sorted vector with periodic merge vs. an adaptive radix tree. Skiplist is the known-good default and supports lock-free reads naturally; the alternatives have better cache behavior but much harder concurrency.
2. **Compression** — LZ4 at block level is ~50 lines of glue but changes every benchmark number and adds a CPU/IO tradeoff worth charting. Include it or declare it out of scope explicitly; do not half-build it.
3. **SSTable reads: `pread` vs. `mmap`.** `mmap` is simpler and fast for cached data but makes I/O error handling and fault injection much harder. Recommend `pread`, and say why in the README.
4. **Compaction threading** — single background thread vs. a pool with subcompactions. A pool roughly doubles the complexity of version installation; probably a stretch goal, but the plan should say where the seams go.
5. **Block cache eviction** — LRU vs. CLOCK vs. TinyLFU. LRU is the baseline; implementing a second policy and charting hit rates against it is a cheap, high-signal addition if time allows.
6. **Whether to add an `io_uring` read path** behind the `Env` interface as an optional backend. Large scope, but it connects this project to lower-level systems work and would make a strong second benchmark chart. Treat as a post-v1 extension, not part of the 4 weeks.
7. **Bloom filter placement** — whole-file (LevelDB) vs. per-block (RocksDB partitioned). Whole-file is simpler; per-block is why RocksDB wins cold reads. Recommend whole-file for v1 and name the gap in the benchmark analysis.

---

## Decisions and corrections (2026-10-01)

Decided: skiplist memtable; compression out of scope for v1 (type byte reserved, only `kNoCompression` accepted); `pread`; single background thread; LRU only; no `io_uring`; one bloom filter per file. Development is native on macOS arm64 with Linux covered by CI.

Where the implementation departs from the text above:

1. **Group commit ordering (§6.2).** The leader stamps the group's starting sequence into the batch header under the mutex before the WAL append, because the WAL record carries it. After the memtable apply it publishes `last_sequence_`.
2. **Old WAL is fsynced at memtable switch.** Otherwise unsynced writes in log N can be lost while synced writes in log N+1 survive, and the recovered state is not a prefix of the history.
3. **A truncated tail is tolerated only in the newest log.** Older logs are fully synced, so damage there is corruption.
4. **`SyncDir` covers every new file**, not only `CURRENT`: before a manifest edit references a new SSTable or log, and before a sync write into a new WAL is acknowledged.
5. **Sticky background error.** After a failed WAL append, WAL sync, or manifest write, all later writes are rejected.
6. **`sync=false` still flushes to the OS before acknowledging**, which is what makes writes survive a process crash.
7. **The crash loop is in-process and seeded** (the filesystem dies at the k-th Env operation). A separate fork + `SIGKILL` test covers the process-crash contract.
8. **ASan and UBSan share one build** in the CI matrix.
