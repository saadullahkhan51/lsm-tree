#include "db/version_set.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "db/filename.h"
#include "db/log_reader.h"
#include "db/table_cache.h"

namespace lsm {

size_t FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files,
                Slice key) {
  size_t left = 0;
  size_t right = files.size();
  while (left < right) {
    const size_t mid = (left + right) / 2;
    if (icmp.Compare(files[mid]->largest.Encode(), key) < 0) {
      // Everything at or before mid ends before key.
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return right;
}

// ---------------------------------------------------------------- Version

Version::~Version() {
  assert(refs_ == 0);

  // Remove from the linked list.
  prev_->next_ = next_;
  next_->prev_ = prev_;

  for (auto& level_files : files_) {
    for (FileMetaData* f : level_files) {
      assert(f->refs > 0);
      if (--f->refs == 0) delete f;
    }
  }
}

void Version::Ref() { ++refs_; }

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  if (--refs_ == 0) delete this;
}

namespace {

// State threaded through TableCache::Get into SaveValue.
enum SaverState { kNotFound, kFound, kDeleted, kCorrupt };

struct Saver {
  SaverState state;
  const Comparator* ucmp;
  Slice user_key;
  std::string* value;
};

// Called with the first entry at or after the lookup key. It decides the
// lookup only if it is for the same user key: then it is the newest version
// visible at the lookup's sequence.
void SaveValue(void* arg, Slice ikey, Slice v) {
  Saver* s = static_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
    s->state = parsed_key.type == kTypeValue ? kFound : kDeleted;
    if (s->state == kFound) s->value->assign(v);
  }
}

}  // namespace

Status Version::Get(const ReadOptions& options, const LookupKey& key, std::string* value,
                    GetStats* stats) const {
  const Slice ikey = key.internal_key();
  const Slice user_key = key.user_key();
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  Saver saver{kNotFound, ucmp, user_key, value};
  // Probes one file. Returns true if the search is over, with *result set.
  auto probe = [&](const FileMetaData* f, Status* result) {
    stats->files_probed++;
    saver.state = kNotFound;
    Status s = vset_->table_cache_->Get(options, f->number, f->file_size, ikey, &saver, SaveValue);
    if (!s.ok()) {
      *result = s;
      return true;
    }
    switch (saver.state) {
      case kNotFound:
        return false;  // keep searching in older files
      case kFound:
        *result = Status::OK();
        return true;
      case kDeleted:
        *result = Status::NotFound({});
        return true;
      case kCorrupt:
        *result = Status::Corruption("corrupted key for ", user_key);
        return true;
    }
    return false;
  };

  Status result;

  // Level 0 files overlap one another, so every file whose range covers the
  // key is a candidate. They are ordered newest first, and the first one
  // with an entry for the key wins.
  for (const FileMetaData* f : files_[0]) {
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      if (probe(f, &result)) return result;
    }
  }

  // Deeper levels are sorted and disjoint: at most one file per level can
  // hold the key, found by binary search.
  for (int level = 1; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = files_[level];
    if (files.empty()) continue;
    const size_t index = FindFile(vset_->icmp_, files, ikey);
    if (index >= files.size()) continue;
    const FileMetaData* f = files[index];
    if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) continue;  // falls in a gap
    if (probe(f, &result)) return result;
  }

  return Status::NotFound({});
}

Status Version::CheckInvariants() const {
  const InternalKeyComparator& icmp = vset_->icmp_;
  const Comparator* ucmp = icmp.user_comparator();
  std::set<uint64_t> numbers;

  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      const std::string where =
          "level " + std::to_string(level) + " file " + std::to_string(f->number);
      if (!numbers.insert(f->number).second) {
        return Status::Corruption(where, "appears more than once in the version");
      }
      if (icmp.Compare(f->smallest.Encode(), f->largest.Encode()) > 0) {
        return Status::Corruption(where, "smallest key is after largest key");
      }
      if (i == 0) continue;

      const FileMetaData* prev = files[i - 1];
      if (level == 0) {
        if (prev->number <= f->number) {
          return Status::Corruption(where, "level 0 is not ordered newest first");
        }
      } else if (ucmp->Compare(prev->largest.user_key(), f->smallest.user_key()) >= 0) {
        return Status::Corruption(
            where, "overlaps file " + std::to_string(prev->number) + ": " +
                       DebugInternalKey(prev->largest.Encode()) + " vs " +
                       DebugInternalKey(f->smallest.Encode()));
      }
    }
  }
  return Status::OK();
}

std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < config::kNumLevels; level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' @ 5 : value .. 'd' @ 9 : value]
    r += "--- level " + std::to_string(level) + " ---\n";
    for (const FileMetaData* f : files_[level]) {
      r += " " + std::to_string(f->number) + ":" + std::to_string(f->file_size) + "[" +
           DebugInternalKey(f->smallest.Encode()) + " .. " +
           DebugInternalKey(f->largest.Encode()) + "]\n";
    }
  }
  return r;
}

// ---------------------------------------------------------------- Builder

// Accumulates a sequence of edits and applies them to a base version
// without materializing the intermediate versions.
class VersionSet::Builder {
 public:
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    base_->Ref();
    for (auto& level : levels_) {
      level.added_files = std::make_unique<FileSet>(BySmallestKey{&vset_->icmp_});
    }
  }

  ~Builder() {
    for (auto& level : levels_) {
      for (FileMetaData* f : *level.added_files) {
        if (--f->refs == 0) delete f;
      }
    }
    base_->Unref();
  }

  void Apply(const VersionEdit* edit) {
    for (const auto& [level, key] : edit->compact_pointers_) {
      vset_->compact_pointer_[level].assign(key.Encode());
    }
    for (const auto& [level, number] : edit->deleted_files_) {
      levels_[level].deleted_files.insert(number);
    }
    for (const auto& [level, meta] : edit->new_files_) {
      auto* f = new FileMetaData(meta);
      f->refs = 1;
      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files->insert(f);
    }
  }

  // Stores the base version with all applied edits into *v.
  void SaveTo(Version* v) {
    const BySmallestKey cmp{&vset_->icmp_};
    for (int level = 0; level < config::kNumLevels; level++) {
      // Merge the added files into the base files, dropping deleted ones.
      const std::vector<FileMetaData*>& base_files = base_->files_[level];
      const FileSet& added = *levels_[level].added_files;
      std::vector<FileMetaData*> merged;
      merged.reserve(base_files.size() + added.size());
      if (level == 0) {
        // Level 0 is kept in file-number order (sorted below), not key order.
        merged = base_files;
        merged.insert(merged.end(), added.begin(), added.end());
      } else {
        std::merge(base_files.begin(), base_files.end(), added.begin(), added.end(),
                   std::back_inserter(merged), cmp);
      }

      std::vector<FileMetaData*>* files = &v->files_[level];
      for (FileMetaData* f : merged) {
        if (levels_[level].deleted_files.count(f->number) > 0) continue;
        f->refs++;
        files->push_back(f);
      }
    }
    // Level 0 is searched newest file first.
    std::sort(v->files_[0].begin(), v->files_[0].end(),
              [](const FileMetaData* a, const FileMetaData* b) { return a->number > b->number; });
  }

 private:
  struct BySmallestKey {
    const InternalKeyComparator* icmp;
    bool operator()(const FileMetaData* a, const FileMetaData* b) const {
      const int r = icmp->Compare(a->smallest.Encode(), b->smallest.Encode());
      return r != 0 ? r < 0 : a->number < b->number;  // break ties by file number
    }
  };

  using FileSet = std::set<FileMetaData*, BySmallestKey>;
  struct LevelState {
    std::set<uint64_t> deleted_files;
    std::unique_ptr<FileSet> added_files;
  };

  VersionSet* vset_;
  Version* base_;
  LevelState levels_[config::kNumLevels];
};

// ------------------------------------------------------------- VersionSet

VersionSet::VersionSet(const std::string& dbname, Env* env, TableCache* table_cache,
                       const InternalKeyComparator* icmp)
    : env_(env), dbname_(dbname), table_cache_(table_cache), icmp_(*icmp), dummy_versions_(this) {
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);  // a version is still referenced
}

void VersionSet::AppendVersion(Version* v) {
  assert(v->refs_ == 0);
  assert(v != current_);

#ifndef NDEBUG
  // Debug builds verify the level structure of every version installed.
  if (Status s = v->CheckInvariants(); !s.ok()) {
    std::fprintf(stderr, "version invariant violated: %s\n%s", s.ToString().c_str(),
                 v->DebugString().c_str());
    std::abort();
  }
#endif

  if (current_ != nullptr) current_->Unref();
  current_ = v;
  v->Ref();

  // Append to the linked list.
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

uint64_t VersionSet::NumLevelBytes(int level) const {
  uint64_t sum = 0;
  for (const FileMetaData* f : current_->files_[level]) sum += f->file_size;
  return sum;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
    for (const auto& level_files : v->files_) {
      for (const FileMetaData* f : level_files) live->insert(f->number);
    }
  }
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());
  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
    for (const FileMetaData* f : current_->files_[level]) {
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }
  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock) {
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }

  // The first edit after opening starts a new manifest, beginning with a
  // snapshot of the current state. The old manifest stays authoritative
  // until CURRENT is switched below.
  Status s;
  std::string new_manifest_file;
  if (descriptor_log_ == nullptr) {
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = std::make_unique<log::Writer>(descriptor_file_.get());
      s = WriteSnapshot(descriptor_log_.get());
    }
  }

  lock->unlock();
  // The files this edit adds exist and their contents are synced, but a
  // file is not durable until its directory entry is. Sync the directory
  // before the manifest says the files are part of the database.
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (s.ok()) {
    std::string record;
    edit->EncodeTo(&record);
    s = descriptor_log_->AddRecord(record);
  }
  if (s.ok()) s = descriptor_file_->Sync();
  if (s.ok() && !new_manifest_file.empty()) {
    s = SetCurrentFile(env_, dbname_, manifest_file_number_);
  }
  lock->lock();

  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number_;
  } else {
    delete v;
    if (!new_manifest_file.empty()) {
      descriptor_log_.reset();
      descriptor_file_.reset();
      (void)env_->RemoveFile(new_manifest_file);
    }
  }
  return s;
}

Status VersionSet::CreateNew(const std::string& dbname, Env* env,
                             const Comparator* user_comparator) {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname, 1);
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(manifest, &file);
  if (!s.ok()) return s;
  {
    log::Writer log(file.get());
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) s = file->Sync();
    if (s.ok()) s = file->Close();
  }
  file.reset();
  if (s.ok()) {
    // Makes the manifest and CURRENT durable together.
    s = SetCurrentFile(env, dbname, 1);
  } else {
    (void)env->RemoveFile(manifest);
  }
  return s;
}

Status VersionSet::Recover() {
  // CURRENT holds the name of the live manifest, followed by a newline.
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) return s;
  if (current.empty() || current.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with a newline");
  }
  current.pop_back();

  const std::string manifest_name = dbname_ + "/" + current;
  std::unique_ptr<SequentialFile> file;
  s = env_->NewSequentialFile(manifest_name, &file);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      return Status::Corruption("CURRENT points to a manifest that does not exist", manifest_name);
    }
    return s;
  }

  bool have_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  Builder builder(this, current_);

  {
    log::Reader reader(file.get());
    Slice record;
    std::string scratch;
    while (s.ok() && reader.ReadRecord(&record, &scratch)) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (s.ok() && edit.has_comparator_ &&
          edit.comparator_ != icmp_.user_comparator()->Name()) {
        s = Status::InvalidArgument(
            edit.comparator_ + " does not match the comparator the database was opened with",
            icmp_.user_comparator()->Name());
      }
      if (!s.ok()) break;

      builder.Apply(&edit);
      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }
      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }
      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }
    }
    // A manifest that ends in a partial record is fine: that edit was being
    // written when the process died and never took effect. Damage is not.
    if (s.ok() && !reader.status().ok()) {
      s = reader.status().IsCorruption()
              ? Status::Corruption(manifest_name, reader.status().ToString())
              : reader.status();
    }
  }
  file.reset();

  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption(manifest_name, "no next-file entry");
    } else if (!have_log_number) {
      s = Status::Corruption(manifest_name, "no log-number entry");
    } else if (!have_last_sequence) {
      s = Status::Corruption(manifest_name, "no last-sequence entry");
    }
  }

  Version* v = nullptr;
  if (s.ok()) {
    v = new Version(this);
    builder.SaveTo(v);
    // A manifest can decode cleanly and still describe an impossible level
    // structure. Refuse it rather than serve reads from it.
    s = v->CheckInvariants();
    if (!s.ok()) {
      // Unpin the files before the version goes away.
      v->Ref();
      v->Unref();
      v = nullptr;
      s = Status::Corruption(manifest_name, s.ToString());
    }
  }

  if (s.ok()) {
    AppendVersion(v);
    // The next manifest takes next_file as its number.
    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    MarkFileNumberUsed(log_number);
  }
  return s;
}

}  // namespace lsm
