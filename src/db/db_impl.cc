#include "db/db_impl.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "db/builder.h"
#include "db/compaction_check.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "table/merger.h"
#include "table/table_builder.h"

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

// A request to compact one level's files in a key range, handed to the
// background thread.
struct DBImpl::ManualCompaction {
  int level;
  bool done = false;
  bool in_progress = false;  // the background thread is working on it
  const InternalKey* begin;  // null means the beginning of the key range
  const InternalKey* end;    // null means the end of the key range
  InternalKey tmp_storage;   // where the background thread keeps its progress
};

// The state of a compaction in progress.
struct DBImpl::CompactionState {
  // A table being produced.
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };

  explicit CompactionState(Compaction* c) : compaction(c) {}

  Output* current_output() { return &outputs.back(); }

  Compaction* const compaction;

  // Entries at or below this sequence are visible to no snapshot once a
  // newer entry for the same key exists, and may be dropped.
  SequenceNumber smallest_snapshot = 0;
  // Sequence numbers readers may still read at: every live snapshot, and
  // kMaxSequenceNumber for the latest state. For the debug check.
  std::vector<SequenceNumber> read_points;

  std::vector<Output> outputs;

  // The output being written, if any.
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
};

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
      versions_(dbname, &options_, &table_cache_, &internal_comparator_) {}

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

bool DBImpl::HasBackgroundWork() const {
  return imm_ != nullptr || manual_compaction_ != nullptr || versions_.NeedsCompaction();
}

void DBImpl::BackgroundThread() {
  Lock lock(mutex_);
  while (true) {
    // After an error nothing more is attempted: the on-disk state can no
    // longer be reasoned about until the database is reopened.
    bg_work_cv_.wait(lock,
                     [this] { return shutting_down_ || (bg_error_.ok() && HasBackgroundWork()); });
    if (shutting_down_) break;
    BackgroundCompaction(&lock);
    bg_finished_cv_.notify_all();
  }
}

SequenceNumber DBImpl::SmallestSnapshot() const {
  // With no snapshots, only the latest state can be read through the
  // current version. (A read in progress has pinned the version it started
  // with and is unaffected by what a compaction installs.)
  return snapshots_.empty() ? versions_.LastSequence() : snapshots_.oldest()->sequence();
}

void DBImpl::BackgroundCompaction(Lock* lock) {
  if (imm_ != nullptr) {
    CompactMemTable(lock);
    return;
  }

  ManualCompaction* const m = manual_compaction_;
  const bool is_manual = m != nullptr;
  std::unique_ptr<Compaction> c;
  InternalKey manual_end;
  if (is_manual) {
    m->in_progress = true;
    c.reset(versions_.CompactRange(m->level, m->begin, m->end));
    m->done = c == nullptr;
    if (c != nullptr) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
  } else {
    c.reset(versions_.PickCompaction());
  }

  Status status;
  if (c == nullptr) {
    // Nothing to do.
  } else if (!is_manual && c->IsTrivialMove()) {
    // The file overlaps nothing in the next level: move it down by editing
    // the manifest, without rewriting it. (A manual compaction rewrites
    // anyway, to shed overwritten and deleted entries.)
    const FileMetaData* f = c->input(0, 0);
    c->edit()->RemoveFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest, f->largest);
    status = versions_.LogAndApply(c->edit(), lock);
  } else {
    CompactionState compact(c.get());
    status = DoCompactionWork(&compact, lock);
    CleanupCompaction(&compact);
    c->ReleaseInputs();
    if (status.ok()) RemoveObsoleteFiles(lock);
  }
  c.reset();

  if (!status.ok() && !shutting_down_) {
    RecordBackgroundError(status);
  }

  if (is_manual) {
    if (!status.ok()) m->done = true;
    if (!m->done) {
      // Only part of the range was compacted. Resume after it next time.
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    m->in_progress = false;
    manual_compaction_ = nullptr;
  }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  if (compact->builder != nullptr) {
    // The compaction ended mid-file.
    compact->builder->Abandon();
    compact->builder.reset();
    compact->outfile.reset();
  }
  for (const CompactionState::Output& out : compact->outputs) {
    pending_outputs_.erase(out.number);
  }
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact->builder == nullptr);
  uint64_t file_number;
  {
    Lock lock(mutex_);
    file_number = versions_.NewFileNumber();
    pending_outputs_.insert(file_number);
    compact->outputs.push_back(CompactionState::Output{file_number, 0, {}, {}});
  }
  Status s = env_->NewWritableFile(TableFileName(dbname_, file_number), &compact->outfile);
  if (s.ok()) {
    compact->builder = std::make_unique<TableBuilder>(table_options_, compact->outfile.get());
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact) {
  assert(compact->builder != nullptr);
  assert(compact->builder->NumEntries() > 0);
  CompactionState::Output* out = compact->current_output();

  Status s = compact->builder->Finish();
  out->file_size = compact->builder->FileSize();
  compact->builder.reset();

  if (s.ok()) s = compact->outfile->Sync();
  if (s.ok()) s = compact->outfile->Close();
  compact->outfile.reset();

  if (s.ok()) {
    // Check that the table can be opened before the inputs it replaces are
    // given up.
    std::unique_ptr<Iterator> iter(
        table_cache_.NewIterator(ReadOptions(), out->number, out->file_size));
    s = iter->status();
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact, Lock* lock) {
  // One edit removes every input and adds every output, so the manifest
  // never describes a state with only part of the compaction applied.
  Compaction* c = compact->compaction;
  c->AddInputDeletions(c->edit());
  for (const CompactionState::Output& out : compact->outputs) {
    c->edit()->AddFile(c->level() + 1, out.number, out.file_size, out.smallest, out.largest);
  }
  return versions_.LogAndApply(c->edit(), lock);
}

Status DBImpl::DoCompactionWork(CompactionState* compact, Lock* lock) {
  Compaction* const c = compact->compaction;
  assert(versions_.NumLevelFiles(c->level()) > 0);
  assert(compact->builder == nullptr);

  compact->smallest_snapshot = SmallestSnapshot();
  snapshots_.ForEach([&](SequenceNumber s) { compact->read_points.push_back(s); });
  compact->read_points.push_back(kMaxSequenceNumber);

  std::unique_ptr<Iterator> input(versions_.MakeInputIterator(c));
  const Comparator* ucmp = internal_comparator_.user_comparator();

  // The merge runs without the mutex.
  lock->unlock();

  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  // Sequence of the previous entry seen for current_user_key, or
  // kMaxSequenceNumber if this is the first (newest) one.
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  // The current output should be finished at the next user key.
  bool roll_at_next_user_key = false;

  for (input->SeekToFirst(); input->Valid() && !shutting_down_; input->Next()) {
    // A waiting memtable flush goes first: writers may be stalled on it.
    if (has_imm_.load(std::memory_order_relaxed)) {
      lock->lock();
      if (imm_ != nullptr) {
        CompactMemTable(lock);
        bg_finished_cv_.notify_all();
      }
      lock->unlock();
    }

    const Slice key = input->key();
    if (!ParseInternalKey(key, &ikey)) {
      // The block passed its checksum, so this is a bug or memory damage.
      // Stop: the inputs stay in place and nothing is lost.
      status = Status::Corruption("unparseable internal key during compaction");
      break;
    }

    const bool new_user_key =
        !has_current_user_key || ucmp->Compare(ikey.user_key, current_user_key) != 0;
    if (new_user_key) {
      current_user_key.assign(ikey.user_key);
      has_current_user_key = true;
      last_sequence_for_key = kMaxSequenceNumber;
    }

    // Outputs are cut only between user keys. All versions of a key then
    // sit in one file, which keeps files within a level disjoint by user
    // key: a later compaction that picks one file can never leave an older
    // version of the same key behind in its neighbor.
    if (c->ShouldStopBefore(key)) roll_at_next_user_key = true;
    if (new_user_key && roll_at_next_user_key) {
      if (compact->builder != nullptr) {
        status = FinishCompactionOutputFile(compact);
        if (!status.ok()) break;
      }
      roll_at_next_user_key = false;
    }

    bool drop = false;
    if (last_sequence_for_key <= compact->smallest_snapshot) {
      // A newer entry for this key is visible to every reader, so no one
      // can see this one.
      drop = true;
    } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
               c->IsBaseLevelForKey(ikey.user_key)) {
      // A tombstone with nothing left to hide: no reader can see past it to
      // an older value (the first test), and no deeper level holds one (the
      // second). Older entries for the key in this compaction's own inputs
      // are dropped by the rule above on the next iterations. Without the
      // second test, a value in a deeper level would come back to life.
      drop = true;
    }
    last_sequence_for_key = ikey.sequence;

    if (drop) continue;

    if (compact->builder == nullptr) {
      status = OpenCompactionOutputFile(compact);
      if (!status.ok()) break;
    }
    if (compact->builder->NumEntries() == 0) {
      compact->current_output()->smallest.DecodeFrom(key);
    }
    compact->current_output()->largest.DecodeFrom(key);
    compact->builder->Add(key, input->value());
    if (compact->builder->FileSize() >= c->MaxOutputFileSize()) {
      roll_at_next_user_key = true;
    }
  }

  if (status.ok() && shutting_down_) {
    status = Status::IOError("database closed during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = compact->builder->status();
    if (status.ok()) status = FinishCompactionOutputFile(compact);
  }
  if (status.ok()) status = input->status();
  input.reset();

#ifndef NDEBUG
  if (status.ok()) {
    // A failure to read back the files fails the compaction like any other
    // I/O error. A difference between input and output is a bug: stop the
    // process before the result can be installed.
    std::string violation;
    status = CheckCompaction(compact, &violation);
    if (!violation.empty()) {
      std::fprintf(stderr, "compaction invariant violated: %s\n", violation.c_str());
      std::abort();
    }
  }
#endif

  lock->lock();
  // A flush that failed while this compaction was running has already put
  // the database into its error state.
  if (status.ok()) status = bg_error_;
  if (status.ok()) {
    status = InstallCompactionResults(compact, lock);
  }
  return status;
}

Status DBImpl::CheckCompaction(CompactionState* compact, std::string* violation) {
  Compaction* const c = compact->compaction;
  ReadOptions options;
  options.fill_cache = false;

  std::vector<FileMetaData> metas(compact->outputs.size());
  std::vector<FileMetaData*> output_files;
  for (size_t i = 0; i < metas.size(); i++) {
    metas[i].number = compact->outputs[i].number;
    metas[i].file_size = compact->outputs[i].file_size;
    metas[i].smallest = compact->outputs[i].smallest;
    metas[i].largest = compact->outputs[i].largest;
    output_files.push_back(&metas[i]);
  }

  std::unique_ptr<Iterator> input(versions_.MakeInputIterator(c));
  std::unique_ptr<Iterator> output(versions_.NewConcatenatingIterator(options, &output_files));
  // Asked of the version the compaction read from, independently of the
  // bookkeeping the compaction itself used to decide.
  auto key_may_exist_below = [c](Slice user_key) {
    for (int level = c->level() + 2; level < config::kNumLevels; level++) {
      if (c->input_version()->OverlapInLevel(level, &user_key, &user_key)) return true;
    }
    return false;
  };
  return CheckCompactionOutput(internal_comparator_.user_comparator(), input.get(), output.get(),
                               compact->read_points, key_may_exist_below, violation);
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
    has_imm_.store(false, std::memory_order_relaxed);
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
  const SequenceNumber snapshot =
      options.snapshot != nullptr
          ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence()
          : versions_.LastSequence();
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

namespace {

// What an internal iterator keeps alive.
struct IterState {
  std::mutex* const mu;
  Version* const version;
  MemTable* const mem;
  MemTable* const imm;
};

void CleanupIteratorState(void* arg1, void*) {
  auto* state = static_cast<IterState*>(arg1);
  {
    std::lock_guard lock(*state->mu);
    state->mem->Unref();
    if (state->imm != nullptr) state->imm->Unref();
    state->version->Unref();
  }
  delete state;
}

}  // namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_sequence) {
  Lock lock(mutex_);
  *latest_sequence = versions_.LastSequence();

  std::vector<Iterator*> children;
  children.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    children.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  Version* current = versions_.current();
  current->AddIterators(options, &children);
  current->Ref();

  Iterator* internal_iter = NewMergingIterator(&internal_comparator_, std::move(children));
  // The memtables and the version (and through it, its files) stay alive
  // for as long as the iterator does, whatever is flushed or compacted
  // meanwhile.
  internal_iter->RegisterCleanup(CleanupIteratorState,
                                 new IterState{&mutex_, current, mem_, imm_}, nullptr);
  return internal_iter;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_sequence;
  Iterator* internal_iter = NewInternalIterator(options, &latest_sequence);
  const SequenceNumber sequence =
      options.snapshot != nullptr
          ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence()
          : latest_sequence;
  return NewDBIterator(internal_comparator_.user_comparator(), internal_iter, sequence);
}

const Snapshot* DBImpl::GetSnapshot() {
  Lock lock(mutex_);
  return snapshots_.New(versions_.LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  Lock lock(mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
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
  using Clock = std::chrono::steady_clock;
  bool allow_delay = !force;
  bool stalled = false;
  // Runs wait() and charges the time to the stall counters.
  auto stall = [&](auto wait) {
    const Clock::time_point start = Clock::now();
    wait();
    stall_micros_ +=
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    if (!stalled) stall_count_++;
    stalled = true;
  };

  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (allow_delay && versions_.NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
      // Level 0 is filling faster than it is compacted. Rather than let it
      // reach the hard limit and stop a write for seconds, slow every write
      // down by a millisecond, which also frees CPU for the compaction.
      // Once per write at most.
      stall([&] {
        lock->unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock->lock();
      });
      allow_delay = false;
      continue;
    }
    // An empty memtable is never switched out for being full. Its arena
    // already holds a block, so with a write buffer smaller than that it
    // would look full forever.
    if (!force &&
        (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size || mem_->empty())) {
      return Status::OK();  // there is room
    }
    if (imm_ != nullptr) {
      // The previous memtable is still being flushed, and only one may be
      // pending. Wait for it.
      stall([&] { bg_finished_cv_.wait(*lock); });
      continue;
    }
    if (versions_.NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // Too many level-0 files: switching memtables would add another.
      // Wait for compaction.
      stall([&] { bg_finished_cv_.wait(*lock); });
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
  has_imm_.store(true, std::memory_order_relaxed);
  mem_ = new MemTable(internal_comparator_);
  mem_->Ref();
  MaybeScheduleCompaction();
  return Status::OK();
}

// ---------------------------------------------------- Manual compaction

Status DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  // The deepest level that holds any of the range decides how far down the
  // range has to be pushed.
  int max_level_with_files = 1;
  {
    Lock lock(mutex_);
    const Version* base = versions_.current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) max_level_with_files = level;
    }
  }
  Status s = TEST_FlushMemTable();  // include what is still in memory
  for (int level = 0; level < max_level_with_files && s.ok(); level++) {
    s = TEST_CompactRange(level, begin, end);
  }
  return s;
}

Status DBImpl::TEST_CompactRange(int level, const Slice* begin, const Slice* end) {
  assert(level >= 0 && level + 1 < config::kNumLevels);

  // Bounds that sort before and after every entry for the given user keys.
  InternalKey begin_storage, end_storage;
  ManualCompaction manual;
  manual.level = level;
  manual.begin = nullptr;
  manual.end = nullptr;
  if (begin != nullptr) {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end != nullptr) {
    end_storage = InternalKey(*end, 0, kTypeDeletion);
    manual.end = &end_storage;
  }

  Lock lock(mutex_);
  while (!manual.done && !shutting_down_ && bg_error_.ok()) {
    if (manual_compaction_ == nullptr) {
      // The background thread is free to take it.
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {
      // It is running this compaction, or someone else's.
      bg_finished_cv_.wait(lock);
    }
  }
  // The background thread may still be using `manual`, which lives on this
  // stack frame.
  bg_finished_cv_.wait(lock, [&] { return !manual.in_progress; });
  if (manual_compaction_ == &manual) manual_compaction_ = nullptr;
  return bg_error_;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();
  Lock lock(mutex_);

  static constexpr Slice kNumFilesPrefix = "lsm.num-files-at-level";
  if (property.starts_with(kNumFilesPrefix)) {
    const Slice digits = property.substr(kNumFilesPrefix.size());
    int level = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), level);
    if (ec != std::errc() || end != digits.data() + digits.size() || digits.empty() ||
        level < 0 || level >= config::kNumLevels) {
      return false;
    }
    *value = std::to_string(versions_.NumLevelFiles(level));
    return true;
  }
  if (property == "lsm.stall-micros") {
    *value = std::to_string(stall_micros_);
    return true;
  }
  if (property == "lsm.stall-count") {
    *value = std::to_string(stall_count_);
    return true;
  }
  if (property == "lsm.levels") {
    for (int level = 0; level < config::kNumLevels; level++) {
      const int files = versions_.NumLevelFiles(level);
      if (files == 0) continue;
      *value += "level " + std::to_string(level) + ": " + std::to_string(files) + " files, " +
                std::to_string(versions_.NumLevelBytes(level)) + " bytes\n";
    }
    return true;
  }
  return false;
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
  Version* current = versions_.current();
  Status s = current->CheckInvariants();
  if (!s.ok()) return s;
  current->Ref();
  lock.unlock();

  ReadOptions options;
  options.fill_cache = false;
  for (int level = 0; level < config::kNumLevels && s.ok(); level++) {
    for (const FileMetaData* f : current->files(level)) {
      const std::string where =
          "level " + std::to_string(level) + " file " + std::to_string(f->number);
      std::unique_ptr<Iterator> iter(table_cache_.NewIterator(options, f->number, f->file_size));
      std::string prev;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        if (prev.empty()) {
          if (iter->key() != f->smallest.Encode()) {
            s = Status::Corruption(where, "first key is not the recorded smallest key");
          }
        } else if (internal_comparator_.Compare(prev, iter->key()) >= 0) {
          s = Status::Corruption(where, "keys are out of order");
        }
        if (!s.ok()) break;
        prev.assign(iter->key());
      }
      if (s.ok()) s = iter->status();
      if (s.ok() && prev != f->largest.Encode()) {
        s = Status::Corruption(where, "last key is not the recorded largest key");
      }
      if (!s.ok()) break;
    }
  }

  lock.lock();
  current->Unref();
  return s;
}

Status DBImpl::TEST_WaitForBackgroundWork() {
  Lock lock(mutex_);
  bg_finished_cv_.wait(lock, [this] { return !bg_error_.ok() || !HasBackgroundWork(); });
  return bg_error_;
}

uint64_t DBImpl::TEST_NumBytesAtLevel(int level) {
  Lock lock(mutex_);
  return versions_.NumLevelBytes(level);
}

}  // namespace lsm
