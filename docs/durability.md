# Durability

What a successful write promises, what the engine does to keep the promise,
and how the promise is tested.

## The contract

When `Write`, `Put`, or `Delete` returns OK:

| `WriteOptions::sync` | The process is killed | The machine loses power or the OS crashes |
|---|---|---|
| `true` | The write survives. | The write survives. |
| `false` (default) | The write survives. | The write may be lost. |

Two more promises hold in every case, whatever was lost:

1. **The recovered state is a prefix of the write history.** If write N
   survived, so did every write before it. A crash can cost you the most
   recent unsynced writes; it cannot cost you one from the middle.
2. **A batch is all or nothing.** A `WriteBatch` is one log record, and a
   record that was cut off is discarded whole.

When a write returns an error, whether it took effect is unknown, and the
database refuses all later writes until it is reopened (see
[Errors](#errors)).

## What "synced" means

`WritableFile::Sync` is `fdatasync` on Linux and `fcntl(F_FULLFSYNC)` on
macOS. On macOS, `fsync` only hands the data to the drive; the drive may
still hold it in a volatile cache, and `F_FULLFSYNC` is the call that asks
the drive to flush. It is much slower, and it is the reason the synced-write
numbers measured on a Mac are low.

Syncing a file makes its contents durable. It does not make its name
durable: a newly created or renamed file can vanish in a power failure unless
its directory is synced too. `Env::SyncDir` does that.

## The write path

1. Writers queue. The one at the head becomes the leader and merges the
   batches queued behind it into one group.
2. The leader stamps the group with the next sequence number and appends it
   to the write-ahead log as one record. The log writer hands every record to
   the OS before returning, so a write with `sync=false` is in the page cache
   and survives the death of the process.
3. If any writer in the group asked for `sync`, the leader syncs the log.
4. The leader applies the group to the memtable, then publishes the new last
   sequence number, which is what makes the writes visible to readers.
5. Every writer in the group gets the leader's status.

## The rules that make it hold

Each rule below is one line of code. For each, removing the line makes the
crash loop fail, which `test/tools/mutation_check.py` checks.

| Rule | What goes wrong without it |
|---|---|
| A `sync=true` write syncs the log before it is acknowledged. | The write is lost on power failure. |
| When the memtable is switched, the outgoing log is synced in full before the new log takes writes. | A synced write in the new log survives while unsynced writes before it, in the old log, are lost. The recovered state has a hole. |
| The directory is synced after a new log is created, before any write into it is acknowledged. | The log's contents were synced, but the file itself is gone. |
| A flushed table is synced before the manifest names it. | The manifest points at a table whose contents are missing. |
| A compaction's output tables are synced before the manifest names them. | The same, and the inputs they replaced have been deleted. |
| The directory is synced before a manifest record that names new tables or logs. | The manifest points at files that no longer exist. |
| A manifest record is synced before the version it describes is used. | Files the old version needs get deleted on the strength of an edit that did not survive. |
| The directory is synced after `CURRENT` is renamed into place. | `CURRENT` reverts to an older manifest, or to nothing, after the files it needs are gone. |

Obsolete files are deleted only after the manifest record that stops
referring to them is durable.

## Recovery

1. Take the `LOCK` file. A second process opening the same directory fails.
2. Read `CURRENT` and replay the manifest it names to rebuild the set of
   tables in each level. `CURRENT` must name a manifest. A directory with
   table or log files but no `CURRENT` is refused rather than initialised
   as a new database, because that would orphan the data.
3. Check that every table the manifest names exists.
4. Replay, in order, every log the manifest has not marked as flushed. Each
   batch must start at the sequence number where the previous one ended.
5. A log may end in a cut-off record. That is how a log looks after a crash,
   and the record is dropped. It is accepted only in the last log that has
   any records: earlier logs were synced in full when they were switched out,
   so a short record in one of them is reported as corruption.
6. Write the replayed data out as tables, record that in the manifest, and
   delete the files nothing refers to any more.

Anything else that fails to parse or fails its checksum makes `Open` return
`Corruption`. The engine does not guess at a repair.

## Errors

After a failed log append, log sync, manifest write, flush, or compaction,
the on-disk state is not known: a record may be partly written, and a failed
`fsync` may have dropped pages the kernel will never retry. The database
records the first such error and returns it from every later write. Reads
keep working. Reopening the database runs recovery, which establishes what
actually reached the disk.

One case needed more than that. A manifest whose append failed may end in a
partial record, and a record appended after it would be unreadable, so the
version set also refuses to append to a manifest after a failed write on it.
That bug was found by the I/O error loop; see
[the build log](buildlog-manifest-after-failed-write.md).

## What is not promised

- **Hardware that acknowledges a flush it did not perform.** Nothing above
  the drive can detect that at write time. Checksums catch the damage when it
  is read back.
- **Data on a filesystem mounted with options that weaken `fsync`.**
- **Unsynced writes across an OS crash or power failure.** That is the
  meaning of `sync=false`.

## How it is tested

| Test | What it does |
|---|---|
| `CrashLoop.EveryAcknowledgedSyncWriteSurvivesAndStateIsAPrefix` | Runs a random workload on a simulated filesystem, kills the filesystem at a random operation, reopens, and checks that every acknowledged synced write is present and the state is a prefix of the history. Crash points land inside flushes, compactions, and manifest switches. Every failure replays from its seed. The long run was 50,000 cycles. |
| `CrashLoop.InjectedIOErrorsNeverProduceWrongData` | Makes file operations fail at random, and checks that the database either reports an error or returns correct data, and reopens cleanly. |
| `ProcessCrash.AcknowledgedWritesSurviveSigkill` | Forks a writer, kills it with `SIGKILL`, and checks that every write it acknowledged with `sync=false` is there. |
| `CrashTest.WithoutTheDirectorySyncCurrentCanPointAtNothing` | Makes the simulated filesystem ignore directory syncs and expects recovery to fail, as a check that the simulation can see the difference. |
| `test/tools/mutation_check.py` | Removes each sync in the table above in turn and expects the crash loop to fail. |

The simulated filesystem (`test/fault_injection_env.h`) keeps, per file, how
many bytes have been synced, and per directory, which names have been synced.
At a crash it truncates every file to its synced length plus a random part of
what was not synced, independently for each file, removes files whose names
were not synced, undoes renames that were not synced, and brings back deleted
files whose deletion was not synced. It does not model bit rot (the bit-flip
test does) or a drive that reorders writes within a file.

A real `kill -9` cannot test power loss, because the page cache survives the
process. That is why the crash loop simulates the filesystem and the
`SIGKILL` test is used only for the `sync=false` promise.
