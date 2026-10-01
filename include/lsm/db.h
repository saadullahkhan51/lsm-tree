#pragma once

#include <string>

#include "lsm/iterator.h"
#include "lsm/options.h"
#include "lsm/slice.h"
#include "lsm/status.h"
#include "lsm/write_batch.h"

namespace lsm {

// A handle to the state of the database at one moment. Obtained from
// DB::GetSnapshot and passed in ReadOptions; immutable and safe to share
// between threads.
class Snapshot {
 protected:
  virtual ~Snapshot() = default;
};

// A persistent ordered map from keys to values. Safe for concurrent use
// from multiple threads without external synchronization.
class DB {
 public:
  // Opens the database at path, creating it if it does not exist. On
  // success stores a heap-allocated database in *dbptr, which the caller
  // deletes when done. A database can be open in one process, once, at a
  // time.
  static Status Open(const Options& options, const std::string& path, DB** dbptr);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB() = default;

  // Sets key to value. See WriteOptions::sync for the durability contract.
  Status Put(const WriteOptions& options, const Slice& key, const Slice& value);

  // Removes key. Succeeds whether or not the key existed.
  Status Delete(const WriteOptions& options, const Slice& key);

  // Applies every update in the batch atomically.
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // Stores the value for key in *value. Returns a status for which
  // IsNotFound() is true if there is none.
  virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;

  // Returns an iterator over the contents of the database as of now, or as
  // of options.snapshot. It starts out invalid: call one of its Seek
  // methods first. The caller deletes it, before deleting the database.
  // Writes and compactions that happen while it is live do not affect what
  // it sees.
  virtual Iterator* NewIterator(const ReadOptions& options) = 0;

  // Pins the current state. Reads given this snapshot see exactly what was
  // there when it was taken, whatever is written, deleted, or compacted
  // afterwards. The data it needs is kept until ReleaseSnapshot.
  virtual const Snapshot* GetSnapshot() = 0;

  // The snapshot must not be used afterwards.
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // Reports an aspect of the database's state. Returns false if the
  // property is not recognized. Properties:
  //
  //   "lsm.num-files-at-level<N>"  table files in level N
  //   "lsm.stall-micros"           total time writes spent stalled
  //   "lsm.stall-count"            number of writes that were stalled
  //   "lsm.levels"                 one line per non-empty level: files and bytes
  virtual bool GetProperty(const Slice& property, std::string* value) = 0;

  // Compacts everything in the key range [*begin, *end] down to the
  // deepest level that holds any of it, discarding overwritten and deleted
  // data on the way. A null bound is open: CompactRange(nullptr, nullptr)
  // compacts the whole database.
  virtual Status CompactRange(const Slice* begin, const Slice* end) = 0;
};

}  // namespace lsm
