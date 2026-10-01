#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "lsm/db.h"
#include "lsm/env.h"
#include "table/table_options.h"
#include "util/cache.h"

namespace lsm {

class MemTable;

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;
  bool GetProperty(const Slice& property, std::string* value) override;
  Status CompactRange(const Slice* begin, const Slice* end) override;

  // --- Hooks for tests. ---

  // Freezes the current memtable and waits until it has been written to a
  // table.
  Status TEST_FlushMemTable();
  // Number of table files at each level of the current version.
  int TEST_NumFilesAtLevel(int level);
  // Checks the current version's level structure.
  Status TEST_CheckInvariants();
  // Compacts the files of one level that overlap [*begin, *end] into the
  // next level. Null bounds are open.
  Status TEST_CompactRange(int level, const Slice* begin, const Slice* end);
  // Total size of the table files at a level.
  uint64_t TEST_NumBytesAtLevel(int level);
  // Waits until the background thread has nothing left to do: no pending
  // flush and no level over its target. Returns the background error, if
  // any.
  Status TEST_WaitForBackgroundWork();

 private:
  friend class DB;
  struct CompactionState;
  struct ManualCompaction;
  struct Writer;

  using Lock = std::unique_lock<std::mutex>;

  // Brings the database to the state the manifest and logs describe. Tables
  // written while replaying logs are recorded in *edit, which the caller
  // must apply.
  Status Recover(VersionEdit* edit, Lock* lock);
  // Replays one log into memtables, flushing them to level 0 as they fill.
  // *truncated_earlier says whether an earlier log ended in a partial
  // record, and is updated for this one.
  Status RecoverLogFile(uint64_t number, VersionEdit* edit, SequenceNumber* max_sequence,
                        SequenceNumber* next_sequence, bool* truncated_earlier, Lock* lock);

  // Writes mem to a new level-0 table and records it in *edit. Releases the
  // mutex while writing.
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Lock* lock);

  // Blocks until there is room in the memtable for the leader's write,
  // switching to a new memtable and log if the current one is full (or
  // force is set). REQUIRES: the caller is the write leader.
  Status MakeRoomForWrite(Lock* lock, bool force);
  Status SwitchMemTable(Lock* lock);
  // Merges the batches of the writers queued behind the leader into one.
  // Sets *last_writer to the last writer included.
  WriteBatch* BuildBatchGroup(Writer** last_writer);

  void BackgroundThread();
  bool HasBackgroundWork() const;
  void MaybeScheduleCompaction();
  // Writes the immutable memtable to level 0 and installs it.
  void CompactMemTable(Lock* lock);
  // Runs one compaction: a memtable flush if one is pending, else a manual
  // compaction if one is requested, else whatever the picker chooses.
  void BackgroundCompaction(Lock* lock);
  // Merges the compaction's inputs into new tables in the next level and
  // installs the result. Releases the mutex while merging.
  Status DoCompactionWork(CompactionState* compact, Lock* lock);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact);
  Status InstallCompactionResults(CompactionState* compact, Lock* lock);
  // Debug builds: checks the outputs against the inputs before installing.
  Status CheckCompaction(CompactionState* compact);
  void CleanupCompaction(CompactionState* compact);
  // Oldest sequence number any reader may still read at.
  SequenceNumber SmallestSnapshot() const;
  // Deletes every file the current state no longer needs.
  void RemoveObsoleteFiles(Lock* lock);
  void RecordBackgroundError(const Status& s);

  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const Options options_;
  const std::string dbname_;

  ShardedLRUCache block_cache_;
  const TableOptions table_options_;
  TableCache table_cache_;

  FileLock* db_lock_ = nullptr;

  // Protects everything below except as noted.
  std::mutex mutex_;

  // Inserted into only by the write leader, outside the mutex; the writer
  // queue is what makes that a single writer.
  MemTable* mem_ = nullptr;
  // Frozen and waiting to be written to a table; null if none. At most one.
  MemTable* imm_ = nullptr;
  // imm_ != nullptr, readable without the mutex by a running compaction.
  std::atomic<bool> has_imm_{false};

  // Appended to only by the write leader, outside the mutex.
  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<log::Writer> log_;
  uint64_t logfile_number_ = 0;

  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;  // scratch for BuildBatchGroup

  // Table files being written that no version refers to yet. Protects them
  // from RemoveObsoleteFiles.
  std::set<uint64_t> pending_outputs_;

  VersionSet versions_;

  // The single background thread: flushes the immutable memtable and runs
  // compactions, one at a time.
  std::thread bg_thread_;
  std::condition_variable bg_work_cv_;      // signaled when there is work or at shutdown
  std::condition_variable bg_finished_cv_;  // signaled when a piece of background work ends
  // Written under the mutex; read without it by a running compaction.
  std::atomic<bool> shutting_down_{false};

  // The manual compaction waiting for or being run by the background
  // thread; null if none.
  ManualCompaction* manual_compaction_ = nullptr;

  // Time writers have spent waiting for the background thread, and how
  // many writes had to wait.
  uint64_t stall_micros_ = 0;
  uint64_t stall_count_ = 0;

  // Set when the log or the manifest can no longer be trusted. Every later
  // write fails with it and background work stops.
  Status bg_error_;
};

}  // namespace lsm
