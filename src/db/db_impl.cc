#include "db/db_impl.h"

#include <algorithm>
#include <condition_variable>
#include <vector>

#include "db/filename.h"
#include "db/log_reader.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"

namespace lsm {

// One call to Write, waiting in the queue.
struct DBImpl::Writer {
  WriteBatch* batch;
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
      mem_(new MemTable(internal_comparator_)) {
  mem_->Ref();
}

DBImpl::~DBImpl() {
  if (logfile_ != nullptr) (void)logfile_->Close();
  mem_->Unref();
  if (db_lock_ != nullptr) (void)env_->UnlockFile(db_lock_);
}

Status DB::Open(const Options& options, const std::string& path, DB** dbptr) {
  *dbptr = nullptr;
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Recover();
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

Status DBImpl::Recover() {
  Status s = env_->CreateDir(dbname_);
  if (!s.ok()) return s;
  s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) return s;

  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) return s;

  std::vector<uint64_t> logs;
  for (const std::string& filename : filenames) {
    uint64_t number;
    FileType type;
    if (ParseFileName(filename, &number, &type) && type == FileType::kLogFile) {
      logs.push_back(number);
    }
  }
  std::sort(logs.begin(), logs.end());

  SequenceNumber next_sequence = 0;
  for (uint64_t number : logs) {
    s = ReplayLogFile(number, &next_sequence);
    if (!s.ok()) return s;
  }

  // Start a new log instead of appending to the last one, which may end in
  // a torn record.
  const uint64_t new_log_number = logs.empty() ? 1 : logs.back() + 1;
  s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &logfile_);
  if (!s.ok()) return s;
  // A new file's directory entry is not durable until the directory is
  // synced. Do it now, or a synced write to this log could vanish with the
  // file on power loss.
  s = env_->SyncDir(dbname_);
  if (!s.ok()) return s;
  log_ = std::make_unique<log::Writer>(logfile_.get());
  return Status::OK();
}

Status DBImpl::ReplayLogFile(uint64_t number, SequenceNumber* next_sequence) {
  const std::string fname = LogFileName(dbname_, number);
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(fname, &file);
  if (!s.ok()) return s;

  log::Reader reader(file.get());
  Slice record;
  std::string scratch;
  WriteBatch batch;
  while (reader.ReadRecord(&record, &scratch)) {
    if (record.size() < WriteBatchInternal::kHeader) {
      return Status::Corruption(fname, "log record too small");
    }
    WriteBatchInternal::SetContents(&batch, record);

    // Batches are numbered consecutively as they are logged. A gap means a
    // record went missing, and replaying past it would recover a state the
    // database was never in.
    const SequenceNumber sequence = WriteBatchInternal::Sequence(&batch);
    if (*next_sequence != 0 && sequence != *next_sequence) {
      return Status::Corruption(fname, "sequence gap between log records");
    }
    *next_sequence = sequence + WriteBatchInternal::Count(&batch);

    s = WriteBatchInternal::InsertInto(&batch, mem_);
    if (!s.ok()) return s;
    last_sequence_ = std::max(last_sequence_, *next_sequence - 1);
  }

  // A truncated tail is how a log ends after a crash and is not an error.
  // Anything else the reader stopped on is.
  if (reader.status().IsCorruption()) {
    return Status::Corruption(fname, reader.status().ToString());
  }
  return reader.status();
}

Status DBImpl::Get(const ReadOptions&, const Slice& key, std::string* value) {
  std::unique_lock lock(mutex_);
  const SequenceNumber snapshot = last_sequence_;
  MemTable* mem = mem_;
  mem->Ref();
  lock.unlock();

  // The memtable is read without the mutex. The leader may be inserting
  // concurrently, but only entries above the snapshot sequence taken here.
  Status s;
  const LookupKey lookup(key, snapshot);
  if (!mem->Get(lookup, value, &s)) {
    s = Status::NotFound({});
  }

  lock.lock();
  mem->Unref();
  return s;
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer w;
  w.batch = updates;
  w.sync = options.sync;

  std::unique_lock lock(mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.wait(lock);
  }
  if (w.done) {
    return w.status;  // a leader wrote this batch as part of its group
  }

  // This writer is at the head of the queue: it leads the next group.
  Status status = bg_error_;
  Writer* last_writer = &w;
  if (status.ok()) {
    WriteBatch* group = BuildBatchGroup(&last_writer);
    // The sequence goes into the batch header, which is what gets logged,
    // so it must be assigned before the append.
    WriteBatchInternal::SetSequence(group, last_sequence_ + 1);
    const SequenceNumber new_last_sequence = last_sequence_ + WriteBatchInternal::Count(group);

    // Log and apply without the mutex. This is safe: &w stays at the head
    // of the queue, so no other writer can reach this point, and readers
    // cannot see the new entries until last_sequence_ is advanced.
    lock.unlock();
    status = log_->AddRecord(WriteBatchInternal::Contents(group));
    if (status.ok() && options.sync) {
      status = logfile_->Sync();
    }
    if (status.ok()) {
      status = WriteBatchInternal::InsertInto(group, mem_);
    }
    lock.lock();

    if (group == &tmp_batch_) tmp_batch_.Clear();
    if (status.ok()) {
      last_sequence_ = new_last_sequence;
    } else if (bg_error_.ok()) {
      // The log may now hold a partial or unsynced record. Whether this
      // group survives a restart is unknown, so nothing may be written
      // after it.
      bg_error_ = status;
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

// REQUIRES: mutex_ held, writers_ not empty.
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
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

}  // namespace lsm
