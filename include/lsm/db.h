#pragma once

#include <string>

#include "lsm/options.h"
#include "lsm/slice.h"
#include "lsm/status.h"
#include "lsm/write_batch.h"

namespace lsm {

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
};

}  // namespace lsm
