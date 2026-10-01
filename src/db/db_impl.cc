#include "db/db_impl.h"

#include <algorithm>
#include <vector>

#include "db/builder.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"

namespace lsm {

namespace {

// File descriptors kept back from the table cache for the log, the
// manifest, and whatever else the process has open.
constexpr int kNumNonTableCacheFiles = 10;

TableOptions MakeTableOptions(const Options& options, const InternalKeyComparator* icmp,
                              ShardedLRUCache* block_cache) {
  TableOptions result;
  result.comparator = icmp;
  result.block_size = options.block_size;
  result.block_restart_interval = options.block_restart_interval;
  result.bloom_bits_per_key = options.bloom_bits_per_key;
  result.block_cache = block_cache;
  return result;
}

}  // namespace

// One call to Write, waiting in the queue.
struct DBImpl::Writer {
  WriteBatch* batch;  // null for a request that only forces a memtable switch
  bool sync;
  bool done = false;
  Status status;
  std::condition_variable cv;
};

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : env_(options.env),
      internal_comparator_(options.comparator),
      options_(options),
      dbname_(dbname),
      block_cache_(options.block_cache_size),
      table_options_(MakeTableOptions(options, &internal_comparator_, &block_cache_)),
      table_cache_(dbname, table_options_, env_,
                   std::max(options.max_open_files - kNumNonTableCacheFiles, 16)),
      versions_(dbname, env_, &table_cache_, &internal_comparator_) {}

DBImpl::~DBImpl() {
  {
    Lock lock(mutex_);
    shutting_down_ = true;
    bg_work_cv_.notify_all();
  }
  // A flush in progress runs to completion (or fails) before the join.
  if (bg_thread_.joinable()) bg_thread_.join();

  if (logfile_ != nullptr) (void)logfile_->Close();
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  if (db_lock_ != nullptr) (void)env_->UnlockFile(db_lock_);
}

Status DB::Open(const Options& options, const std::string& path, DB** dbptr) {
  *dbptr = nullptr;
  auto impl = std::make_unique<DBImpl>(options, path);

  DBImpl::Lock lock(impl->mutex_);
  VersionEdit edit;
  Status s = impl->Recover(&edit, &lock);
  if (s.ok()) {
    // Start a new log rather than appending to an old one, which may end
    // in a torn record. Everything replayed from the old logs is in level 0
    // by now, so the edit also retires them.
    const uint64_t new_log_number = impl->versions_.NewFileNumber();
    s = impl->env_->NewWritableFile(LogFileName(path, new_log_number), &impl->logfile_);
    if (s.ok()) {
      impl->logfile_number_ = new_log_number;
      impl->log_ = std::make_unique<log::Writer>(impl->logfile_.get());
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
      edit.SetLogNumber(new_log_number);
      // Also syncs the directory, making the new log's entry durable before
      // any write to it can be acknowledged.
      s = impl->versions_.LogAndApply(&edit, &lock);
    }
  }
  if (s.ok()) {
    impl->RemoveObsoleteFiles(&lock);
    impl->bg_thread_ = std::thread(&DBImpl::BackgroundThread, impl.get());
  }
  lock.unlock();

  if (s.ok()) *dbptr = impl.release();
  return s;
}

Status DB::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DB::Delete(const WriteOptions& options, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

// ------------------------------------------------------------- Recovery

Status DBImpl::Recover(VersionEdit* edit, Lock* lock) {
  Status s = env_->CreateDir(dbname_);
  if (!s.ok()) return s;
  s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) return s;

  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) return s;

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    // No CURRENT: a new database, unless the directory already holds data.
    // Creating a fresh manifest then would orphan that data, and the next
    // cleanup would delete it.
    for (const std::string& filename : filenames) {
      uint64_t number;
      FileType type;
      if (ParseFileName(filename, &number, &type) &&
          (type == FileType::kLogFile || type == FileType::kTableFile)) {
        return Status::Corruption(dbname_, "CURRENT is missing but data files exist: " + filename);
      }
    }
    s = VersionSet::CreateNew(dbname_, env_, internal_comparator_.user_comparator());
    if (!s.ok()) return s;
  }

  s = versions_.Recover();
  if (!s.ok()) return s;

  // Every table the manifest names must exist, and the logs written since
  // the last flush need to be replayed.
  std::set<uint64_t> expected;
  versions_.AddLiveFiles(&expected);
  std::vector<uint64_t> logs;
  for (const std::string& filename : filenames) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(filename, &number, &type)) continue;
    if (type == FileType::kTableFile) {
      expected.erase(number);
    } else if (type == FileType::kLogFile && number >= versions_.LogNumber()) {
      logs.push_back(number);
    }
  }
  if (!expected.empty()) {
    return Status::Corruption(std::to_string(expected.size()) + " table file(s) missing, e.g.",
                              TableFileName(dbname_, *expected.begin()));
  }

  std::sort(logs.begin(), logs.end());
  SequenceNumber max_sequence = 0;
  SequenceNumber next_sequence = 0;
  bool truncated_earlier = false;
  for (uint64_t number : logs) {
    s = RecoverLogFile(number, edit, &max_sequence, &next_sequence, &truncated_earlier, lock);
    if (!s.ok()) return s;
    // The manifest may not have recorded this number as allocated.
    versions_.MarkFileNumberUsed(number);
  }

  if (versions_.LastSequence() < max_sequence) {
    versions_.SetLastSequence(max_sequence);
  }
  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t number, VersionEdit* edit, SequenceNumber* max_sequence,
                              SequenceNumber* next_sequence, bool* truncated_earlier,
                              Lock* lock) {
  const std::string fname = LogFileName(dbname_, number);
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(fname, &file);
  if (!s.ok()) return s;

  log::Reader reader(file.get());
  Slice record;
  std::string scratch;
  WriteBatch batch;
  MemTable* mem = nullptr;
  while (s.ok() && reader.ReadRecord(&record, &scratch)) {
    if (*truncated_earlier) {
      // An earlier log lost its tail, yet this later one has records.
      // Replaying both would leave a hole in the history. This cannot
      // happen after a crash, because a log is synced in full before the
      // next one is written to; it means the earlier log is damaged.
      s = Status::Corruption(fname, "follows a log that ends in an incomplete record");
      break;
    }
    if (record.size() < WriteBatchInternal::kHeader) {
      s = Status::Corruption(fname, "log record too small");
      break;
    }
    WriteBatchInternal::SetContents(&batch, record);

    // Batches are numbered consecutively as they are logged. A gap means a
    // record went missing, and replaying past it would recover a state the
    // database was never in.
    const SequenceNumber sequence = WriteBatchInternal::Sequence(&batch);
    if (*next_sequence != 0 && sequence != *next_sequence) {
      s = Status::Corruption(fname, "sequence gap between log records");
      break;
    }
    *next_sequence = sequence + WriteBatchInternal::Count(&batch);

    if (mem == nullptr) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }
    s = WriteBatchInternal::InsertInto(&batch, mem);
    if (!s.ok()) break;
    *max_sequence = std::max(*max_sequence, *next_sequence - 1);

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      s = WriteLevel0Table(mem, edit, lock);
      mem->Unref();
      mem = nullptr;
    }
  }

  // A truncated tail is how a log ends after a crash and is not an error.
  // Anything else the reader stopped on is.
  if (s.ok() && !reader.status().ok()) {
    s = reader.status().IsCorruption() ? Status::Corruption(fname, reader.status().ToString())
                                       : reader.status();
  }
  if (s.ok() && reader.tail_truncated()) *truncated_earlier = true;

  if (s.ok() && mem != nullptr) {
    s = WriteLevel0Table(mem, edit, lock);
  }
  if (mem != nullptr) mem->Unref();
  return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Lock* lock) {
  FileMetaData meta;
  meta.number = versions_.NewFileNumber();
  pending_outputs_.insert(meta.number);
  std::unique_ptr<Iterator> iter(mem->NewIterator());

  lock->unlock();
  Status s = BuildTable(dbname_, env_, table_options_, &table_cache_, iter.get(), &meta);
  lock->lock();

  iter.reset();
  pending_outputs_.erase(meta.number);

  // A file size of zero means the memtable was empty and no file exists.
  if (s.ok() && meta.file_size > 0) {
    edit->AddFile(0, meta.number, meta.file_size, meta.smallest, meta.largest);
  }
  return s;
}

// ---------------------------------------------------------- Background

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    bg_finished_cv_.notify_all();  // writers waiting on a flush must see it
  }
}

void DBImpl::MaybeScheduleCompaction() { bg_work_cv_.notify_one(); }

void DBImpl::BackgroundThread() {
  Lock lock(mutex_);
  while (true) {
    bg_work_cv_.wait(lock,
                     [this] { return shutting_down_ || (bg_error_.ok() && imm_ != nullptr); });
    if (shutting_down_) break;
    CompactMemTable(&lock);
    bg_finished_cv_.notify_all();
  }
}

void DBImpl::CompactMemTable(Lock* lock) {
  assert(imm_ != nullptr);

  VersionEdit edit;
  Status s = WriteLevel0Table(imm_, &edit, lock);
  if (s.ok()) {
    // Everything in logs older than the current one is in this table now.
    edit.SetLogNumber(logfile_number_);
    s = versions_.LogAndApply(&edit, lock);
  }

  if (s.ok()) {
    imm_->Unref();
    imm_ = nullptr;
    RemoveObsoleteFiles(lock);
  } else {
    RecordBackgroundError(s);
  }
}

void DBImpl::RemoveObsoleteFiles(Lock* lock) {
  if (!bg_error_.ok()) {
    // After a failed manifest write it is unknown whether the new version
    // took effect, so it is unknown which files are garbage.
    return;
  }

  std::set<uint64_t> live = pending_outputs_;
  versions_.AddLiveFiles(&live);

  std::vector<std::string> filenames;
  if (!env_->GetChildren(dbname_, &filenames).ok()) return;  // try again next time

  std::vector<std::string> to_delete;
  for (const std::string& filename : filenames) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(filename, &number, &type)) continue;

    bool keep = true;
    switch (type) {
      case FileType::kLogFile:
        keep = number >= versions_.LogNumber();
        break;
      case FileType::kDescriptorFile:
        // Keep the live manifest and any newer one being written.
        keep = number >= versions_.ManifestFileNumber();
        break;
      case FileType::kTableFile:
        keep = live.count(number) > 0;
        break;
      case FileType::kTempFile:
        // A temp file in use is in pending_outputs_ under its number.
        keep = live.count(number) > 0;
        break;
      case FileType::kCurrentFile:
      case FileType::kLockFile:
        keep = true;
        break;
    }
    if (!keep) {
      if (type == FileType::kTableFile) table_cache_.Evict(number);
      to_delete.push_back(filename);
    }
  }

  // Nothing else can pick these names up, so the unlinks need no lock.
  lock->unlock();
  for (const std::string& filename : to_delete) {
    (void)env_->RemoveFile(dbname_ + "/" + filename);
  }
  lock->lock();
}

// ---------------------------------------------------------------- Reads

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  Lock lock(mutex_);
  const SequenceNumber snapshot = versions_.LastSequence();
  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_.current();
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();
  lock.unlock();

  // Everything below runs without the mutex. Newest data first: the first
  // source with an entry for the key decides, whether it is a value or a
  // tombstone.
  Status s;
  const LookupKey lookup(key, snapshot);
  if (mem->Get(lookup, value, &s)) {
    // Done.
  } else if (imm != nullptr && imm->Get(lookup, value, &s)) {
    // Done.
  } else {
    Version::GetStats stats;
    s = current->Get(options, lookup, value, &stats);
  }

  lock.lock();
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();
  return s;
}

// --------------------------------------------------------------- Writes

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer w;
  w.batch = updates;
  w.sync = options.sync;

  Lock lock(mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.wait(lock);
  }
  if (w.done) {
    return w.status;  // a leader wrote this batch as part of its group
  }

  // This writer is at the head of the queue: it leads the next group.
  Status status = MakeRoomForWrite(&lock, /*force=*/updates == nullptr);
  Writer* last_writer = &w;
  if (status.ok() && updates != nullptr) {
    WriteBatch* group = BuildBatchGroup(&last_writer);
    // The sequence goes into the batch header, which is what gets logged,
    // so it must be assigned before the append.
    const SequenceNumber last_sequence = versions_.LastSequence();
    WriteBatchInternal::SetSequence(group, last_sequence + 1);
    const SequenceNumber new_last_sequence = last_sequence + WriteBatchInternal::Count(group);

    // Log and apply without the mutex. This is safe: &w stays at the head
    // of the queue, so no other writer can reach this point, and readers
    // cannot see the new entries until the last sequence is advanced.
    MemTable* mem = mem_;
    lock.unlock();
    status = log_->AddRecord(WriteBatchInternal::Contents(group));
    if (status.ok() && options.sync) {
      status = logfile_->Sync();
    }
    if (status.ok()) {
      status = WriteBatchInternal::InsertInto(group, mem);
    }
    lock.lock();

    if (group == &tmp_batch_) tmp_batch_.Clear();
    if (status.ok()) {
      versions_.SetLastSequence(new_last_sequence);
    } else {
      // The log may now hold a partial or unsynced record. Whether this
      // group survives a restart is unknown, so nothing may be written
      // after it.
      RecordBackgroundError(status);
    }
  }

  // Hand the result to every writer in the group and pop them.
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) break;
  }
  // Wake the next leader.
  if (!writers_.empty()) {
    writers_.front()->cv.notify_one();
  }
  return status;
}

// REQUIRES: mutex_ held, writers_ not empty, the leader's batch is not null.
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);
  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Let the group grow to 1 MiB, but if the leader's own write is small do
  // not make it wait on a much larger group.
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  for (auto it = std::next(writers_.begin()); it != writers_.end(); ++it) {
    Writer* w = *it;
    if (w->sync && !first->sync) {
      // A sync write must not ride in a group that will not be synced.
      break;
    }
    if (w->batch == nullptr) {
      // A request to switch memtables has to lead its own turn.
      break;
    }
    size += WriteBatchInternal::ByteSize(w->batch);
    if (size > max_size) break;

    if (result == first->batch) {
      // Combine into the scratch batch rather than modifying the leader's.
      result = &tmp_batch_;
      assert(WriteBatchInternal::Count(result) == 0);
      WriteBatchInternal::Append(result, first->batch);
    }
    WriteBatchInternal::Append(result, w->batch);
    *last_writer = w;
  }
  return result;
}

Status DBImpl::MakeRoomForWrite(Lock* lock, bool force) {
  assert(!writers_.empty());
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
      return Status::OK();  // there is room
    }
    if (imm_ != nullptr) {
      // The previous memtable is still being flushed, and only one may be
      // pending. Wait for it.
      bg_finished_cv_.wait(*lock);
      continue;
    }
    Status s = SwitchMemTable(lock);
    if (!s.ok()) return s;
    force = false;
  }
}

Status DBImpl::SwitchMemTable(Lock* lock) {
  assert(imm_ == nullptr);
  const uint64_t new_log_number = versions_.NewFileNumber();

  // The I/O runs without the mutex. Only the write leader gets here, so
  // the log is not in use by anyone else.
  lock->unlock();
  std::unique_ptr<WritableFile> new_logfile;
  Status s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &new_logfile);
  // Make the outgoing log durable in full before anything is written to
  // the new one. Otherwise a power failure could keep a synced write in the
  // new log while losing unsynced ones before it in the old log, and the
  // recovered state would have a hole in it.
  if (s.ok()) s = logfile_->Sync();
  if (s.ok()) s = logfile_->Close();
  // And make the new log's directory entry durable before any write to it
  // is acknowledged.
  if (s.ok()) s = env_->SyncDir(dbname_);
  lock->lock();

  if (!s.ok()) {
    // The old log may be closed or half-synced; no more writes.
    RecordBackgroundError(s);
    return s;
  }

  logfile_ = std::move(new_logfile);
  logfile_number_ = new_log_number;
  log_ = std::make_unique<log::Writer>(logfile_.get());
  imm_ = mem_;
  mem_ = new MemTable(internal_comparator_);
  mem_->Ref();
  MaybeScheduleCompaction();
  return Status::OK();
}

// ----------------------------------------------------------- Test hooks

Status DBImpl::TEST_FlushMemTable() {
  // A null batch forces a memtable switch.
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok()) {
    Lock lock(mutex_);
    bg_finished_cv_.wait(lock, [this] { return imm_ == nullptr || !bg_error_.ok(); });
    if (imm_ != nullptr) s = bg_error_;
  }
  return s;
}

int DBImpl::TEST_NumFilesAtLevel(int level) {
  Lock lock(mutex_);
  return versions_.NumLevelFiles(level);
}

Status DBImpl::TEST_CheckInvariants() {
  Lock lock(mutex_);
  return versions_.current()->CheckInvariants();
}

}  // namespace lsm
