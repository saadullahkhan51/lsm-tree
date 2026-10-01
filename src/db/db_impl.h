#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "db/dbformat.h"
#include "db/log_writer.h"
#include "lsm/db.h"
#include "lsm/env.h"

namespace lsm {

class MemTable;

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;

 private:
  friend class DB;
  struct Writer;

  // Locks the directory, replays every log into the memtable, and starts a
  // new log.
  Status Recover();
  // *next_sequence is the sequence the next record must start at, or 0
  // before the first record.
  Status ReplayLogFile(uint64_t number, SequenceNumber* next_sequence);

  // Merges the batches of the writers queued behind the leader into one.
  // Sets *last_writer to the last writer included.
  WriteBatch* BuildBatchGroup(Writer** last_writer);

  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const Options options_;
  const std::string dbname_;

  FileLock* db_lock_ = nullptr;

  // Protects everything below except as noted.
  std::mutex mutex_;

  // Inserted into only by the write leader, outside the mutex; the writer
  // queue is what makes that a single writer.
  MemTable* mem_;

  // Appended to only by the write leader, outside the mutex.
  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<log::Writer> log_;

  // Sequence of the newest write visible to readers.
  SequenceNumber last_sequence_ = 0;

  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;  // scratch for BuildBatchGroup

  // Set when the log can no longer be trusted. Every later write fails
  // with it.
  Status bg_error_;
};

}  // namespace lsm
