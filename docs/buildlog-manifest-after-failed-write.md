# Build log: the write that came after the failed write

The crash loop had just survived 6,000 simulated power failures. Then the
gentler test next to it, the one that only makes I/O calls fail, produced a
database that would not open:

```
crash_loop_test.cc:402: Failure
  Actual: false (Corruption: .../MANIFEST-000603: Corruption: log: checksum mismatch)
database did not reopen once the errors stopped
Google Test trace: seed 1000, round 22
```

No power was lost in that test and nothing was acknowledged that should not
have been. An I/O error had been reported to the caller, correctly. And the
database was still destroyed.

## The test

`CrashLoop.InjectedIOErrorsNeverProduceWrongData` opens the database on a
simulated filesystem in which every operation fails with a small probability,
between 0.03% and 0.33% per call. It writes and reads against a model until a
write fails. Then it turns the errors off, reopens, and checks two things:
the database opens, and it holds exactly the writes that were acknowledged.

The first check failed. The manifest had a bad checksum in the middle.

## What a manifest can and cannot survive

The manifest is a log of version edits: "table 12 was added to level 1,
tables 7 and 9 were removed." Recovery replays it to learn which files make
up the database. It uses the same record format as the write-ahead log, and
that format forgives exactly one kind of damage: a record that is cut off at
the end of the file. That is what an interrupted append looks like, the
record was never acknowledged, and dropping it is correct.

A bad record with good records after it is something else. Recovery cannot
skip it, because it does not know what the lost edit said. Maybe it added a
table that later edits delete. So it reports corruption and stops. That is
the right behaviour, and it is what happened here. The question was how a bad
record got into the middle.

## The sequence

The engine already had the rule that should have prevented this. After any
failed manifest write, the database records a sticky error and refuses all
further writes. The header even said so:

```cpp
// If this fails, the manifest may end in a partial record. The caller
// must not call LogAndApply again.
Status LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock);
```

There is one background thread. A compaction is a long merge, and a full
memtable cannot wait for it, so the merge loop checks on every key whether a
memtable is waiting and, if one is, stops to flush it. The flush writes a
table and then appends an edit to the manifest.

In round 22, the injected error landed on that append:

1. A compaction is partway through its merge.
2. A memtable fills. The merge loop pauses to flush it.
3. The flush's manifest append fails partway. The manifest now ends in a
   partial record. The database records the error; user writes are refused
   from here on.
4. The flush returns to the merge loop, which carries on. Nothing told it to
   stop, and its own I/O was succeeding.
5. The merge finishes and installs its result, which means appending its own
   edit to the manifest, directly after the partial record.

Step 5 turns a cut-off tail, which recovery forgives, into damage in the
middle of the file, which it cannot.

The sticky error did its job for every path that checked it. The compaction
install did not, because when that code was written a compaction could only
fail through its own I/O. The flush in the middle of the merge was a second
way for the world to change underneath it.

## The fix

Two changes, and the first is the one that matters.

The rule moved from a comment into the code that owns the file. `VersionSet`
now remembers the first error from writing the manifest and returns it from
every later `LogAndApply` without touching the file:

```cpp
Status VersionSet::LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock) {
  if (!manifest_error_.ok()) return manifest_error_;
  ...
```

"The caller must not call this again" was a contract that every caller had to
remember, including callers written later. The version of the rule that
cannot be forgotten is the one where the callee refuses.

The second change makes the compaction check the database's error state
before it installs, so that it stops cleanly instead of running into the
refusal.

The regression test,
`VersionSetTest.NothingIsAppendedToTheManifestAfterAFailedWrite`, fails a
manifest write partway, heals the filesystem, tries another edit, and checks
that the edit is refused and the manifest has not grown by a byte.

## What else the same loop found

Two more bugs came out of the same test in the same sitting, both on error
paths that no ordinary test reaches:

- When switching to a new manifest failed, the cleanup deleted the new
  manifest. If the failure came after `CURRENT` had been renamed to point at
  it, that deleted the live manifest. The cleanup now leaves the file alone.
- The debug-build compaction checker re-reads a compaction's output to verify
  that no visible key was dropped. When that read hit an injected error, the
  checker treated "could not read" as "invariant violated" and aborted the
  process. It now reports the two separately.

After the fixes, 5,000 rounds ran clean: 5,685 injected errors, 4,793 failed
writes, 326 failed reads, 196 failed opens, and every reopen succeeded with
the acknowledged data intact.

## Takeaways

**Power-loss testing and error testing find different bugs.** A crash stops
everything at once, so no code runs after the failure. An I/O error leaves
the process alive with one operation failed and every other thread of work
still going. The dangerous code is whatever runs next. This bug could not
have been found by the crash loop, no matter how many cycles it ran.

**The failure is not where the bug is.** The injected error was handled
correctly. The damage was done afterwards, by code whose own operations all
succeeded. Reading the code path that failed would have found
nothing.

**An invariant that lives in a comment has as many enforcement points as it
has callers.** Moving it into the callee gave it one.
