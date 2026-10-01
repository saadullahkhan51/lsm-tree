#include "db/version_set.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "db/filename.h"
#include "db/log_reader.h"
#include "db/table_cache.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace lsm {

namespace {

uint64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  uint64_t sum = 0;
  for (const FileMetaData* f : files) sum += f->file_size;
  return sum;
}

}  // namespace

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

void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) const {
  // Level-0 files overlap, so each is a separate input to the merge.
  for (const FileMetaData* f : files_[0]) {
    iters->push_back(vset_->table_cache_->NewIterator(options, f->number, f->file_size));
  }
  // A deeper level is one sorted run: its files are opened lazily, one at
  // a time, as the iterator reaches them.
  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) {
      iters->push_back(vset_->NewConcatenatingIterator(options, &files_[level]));
    }
  }
}

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

void Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end,
                                   std::vector<FileMetaData*>* inputs) const {
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != nullptr) user_begin = begin->user_key();
  if (end != nullptr) user_end = end->user_key();
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  for (size_t i = 0; i < files_[level].size();) {
    FileMetaData* f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != nullptr && ucmp->Compare(file_limit, user_begin) < 0) {
      // Entirely before the range.
    } else if (end != nullptr && ucmp->Compare(file_start, user_end) > 0) {
      // Entirely after the range.
    } else {
      inputs->push_back(f);
      if (level == 0) {
        // Level-0 files overlap each other. If this file widens the range,
        // files already passed over may now overlap it: start again.
        if (begin != nullptr && ucmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != nullptr && ucmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      }
    }
  }
}

bool Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                             const Slice* largest_user_key) const {
  const Comparator* ucmp = vset_->icmp_.user_comparator();
  // ponytail: linear scan. Levels 1+ are sorted and could be binary
  // searched; this runs once per manual compaction.
  for (const FileMetaData* f : files_[level]) {
    const bool before = smallest_user_key != nullptr &&
                        ucmp->Compare(f->largest.user_key(), *smallest_user_key) < 0;
    const bool after = largest_user_key != nullptr &&
                       ucmp->Compare(f->smallest.user_key(), *largest_user_key) > 0;
    if (!before && !after) return true;
  }
  return false;
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

VersionSet::VersionSet(const std::string& dbname, const Options* options,
                       TableCache* table_cache, const InternalKeyComparator* icmp)
    : options_(options),
      env_(options->env),
      dbname_(dbname),
      table_cache_(table_cache),
      icmp_(*icmp),
      dummy_versions_(this) {
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

  Finalize(v);
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

// ------------------------------------------------------------ Compaction

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  // Level 0 is limited by file count, not bytes; this is not used for it.
  uint64_t result = options_->max_bytes_for_level_base;
  for (int l = 1; l < level; l++) {
    result *= static_cast<uint64_t>(options_->level_size_multiplier);
  }
  return result;
}

void VersionSet::Finalize(Version* v) const {
  // The last level has nowhere to compact to, so it is never scored.
  int best_level = -1;
  double best_score = -1;
  for (int level = 0; level < config::kNumLevels - 1; level++) {
    double score;
    if (level == 0) {
      // Level 0 is scored by file count rather than bytes: every level-0
      // file is another file each read may have to check, however small it
      // is, and with a large write buffer four files already exceed any
      // sensible byte target.
      score = static_cast<double>(v->files_[0].size()) / config::kL0_CompactionTrigger;
    } else {
      score = static_cast<double>(TotalFileSize(v->files_[level])) /
              static_cast<double>(MaxBytesForLevel(level));
    }
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

// Smallest and largest internal key across inputs. REQUIRES: not empty.
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                          InternalKey* largest) const {
  assert(!inputs.empty());
  *smallest = inputs[0]->smallest;
  *largest = inputs[0]->largest;
  for (const FileMetaData* f : inputs) {
    if (icmp_.Compare(f->smallest.Encode(), smallest->Encode()) < 0) *smallest = f->smallest;
    if (icmp_.Compare(f->largest.Encode(), largest->Encode()) > 0) *largest = f->largest;
  }
}

void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2, InternalKey* smallest,
                           InternalKey* largest) const {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

Compaction* VersionSet::PickCompaction() {
  if (!NeedsCompaction()) return nullptr;
  const int level = current_->compaction_level_;
  assert(level >= 0 && level + 1 < config::kNumLevels);

  auto* c = new Compaction(this, level);
  // Take the first file after the point where the last compaction of this
  // level ended, wrapping around, so that successive compactions sweep the
  // whole key space instead of rewriting the same range.
  const std::vector<FileMetaData*>& files = current_->files_[level];
  for (FileMetaData* f : files) {
    if (compact_pointer_[level].empty() ||
        icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
      c->inputs_[0].push_back(f);
      break;
    }
  }
  if (c->inputs_[0].empty()) {
    c->inputs_[0].push_back(files[0]);
  }

  c->input_version_ = current_;
  c->input_version_->Ref();

  if (level == 0) {
    // Level-0 files overlap one another. Leaving behind one that overlaps
    // the chosen file would put older data above newer data.
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  }

  SetupOtherInputs(c);
  return c;
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest, largest;
  GetRange(c->inputs_[0], &smallest, &largest);

  current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);

  // The range covered by the whole compaction.
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  // The level+1 files usually span more than the level files that selected
  // them. Any other level files inside that wider span can join for free:
  // they are merged into files that are being rewritten anyway. Do so as
  // long as it does not pull in more level+1 files.
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
    const uint64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const uint64_t expanded0_size = TotalFileSize(expanded0);
    const uint64_t expanded_limit = 25 * uint64_t{options_->max_file_size};
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size < expanded_limit) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      current_->GetOverlappingInputs(level + 1, &new_start, &new_limit, &expanded1);
      if (expanded1.size() == c->inputs_[1].size()) {
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit, &c->grandparents_);
  }

  // Advance the pointer now rather than when the edit is applied: if this
  // compaction fails, the next attempt tries a different range.
  compact_pointer_[level].assign(largest.Encode());
  c->edit_.SetCompactPointer(level, largest);
}

Compaction* VersionSet::CompactRange(int level, const InternalKey* begin,
                                     const InternalKey* end) {
  std::vector<FileMetaData*> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) return nullptr;

  // Do not compact a huge range in one go; the caller comes back for the
  // rest. Level 0 cannot be cut this way: leaving behind a file that
  // overlaps a chosen one would put older data above newer data.
  if (level > 0) {
    const uint64_t limit = options_->max_file_size;
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
      total += inputs[i]->file_size;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }

  auto* c = new Compaction(this, level);
  c->input_version_ = current_;
  c->input_version_->Ref();
  c->inputs_[0] = inputs;
  SetupOtherInputs(c);
  return c;
}

namespace {

// Walks the files of one sorted level. key() is the file's largest key and
// value() is its number and size as two fixed64s, which is what
// GetFileIterator needs to open it.
class LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>* flist)
      : icmp_(icmp), flist_(flist), index_(flist->size()) {}

  bool Valid() const override { return index_ < flist_->size(); }
  void Seek(Slice target) override { index_ = FindFile(icmp_, *flist_, target); }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override { index_ = flist_->empty() ? 0 : flist_->size() - 1; }
  void Next() override {
    assert(Valid());
    index_++;
  }
  void Prev() override {
    assert(Valid());
    index_ = index_ == 0 ? flist_->size() : index_ - 1;  // size() marks invalid
  }
  Slice key() const override {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }
  Slice value() const override {
    assert(Valid());
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_ + 8, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  Status status() const override { return Status::OK(); }

 private:
  const InternalKeyComparator icmp_;
  const std::vector<FileMetaData*>* const flist_;
  size_t index_;
  mutable char value_buf_[16];  // backing store for value()
};

}  // namespace

Iterator* VersionSet::NewConcatenatingIterator(const ReadOptions& options,
                                               const std::vector<FileMetaData*>* files) const {
  TableCache* table_cache = table_cache_;
  return NewTwoLevelIterator(
      new LevelFileNumIterator(icmp_, files),
      [table_cache](const ReadOptions& read_options, Slice file_value) -> Iterator* {
        if (file_value.size() != 16) {
          return NewErrorIterator(Status::Corruption("bad file entry in level iterator"));
        }
        return table_cache->NewIterator(read_options, DecodeFixed64(file_value.data()),
                                        DecodeFixed64(file_value.data() + 8));
      },
      options);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.fill_cache = false;  // a compaction's blocks are read once

  // Level-0 files overlap, so each needs its own iterator in the merge. A
  // sorted level is one concatenated run.
  std::vector<Iterator*> children;
  for (int which = 0; which < 2; which++) {
    if (c->inputs_[which].empty()) continue;
    if (c->level() + which == 0) {
      for (const FileMetaData* f : c->inputs_[which]) {
        children.push_back(table_cache_->NewIterator(options, f->number, f->file_size));
      }
    } else {
      children.push_back(NewConcatenatingIterator(options, &c->inputs_[which]));
    }
  }
  return NewMergingIterator(&icmp_, std::move(children));
}

Compaction::Compaction(const VersionSet* vset, int level)
    : vset_(vset), level_(level), max_output_file_size_(vset->options_->max_file_size) {}

Compaction::~Compaction() { ReleaseInputs(); }

void Compaction::ReleaseInputs() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
    input_version_ = nullptr;
  }
}

bool Compaction::IsTrivialMove() const {
  // Moving is free, but not if the file would then overlap so much of the
  // level below that compacting it later becomes very expensive.
  return num_input_files(0) == 1 && num_input_files(1) == 0 &&
         TotalFileSize(grandparents_) <= vset_->MaxGrandParentOverlapBytes();
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (const FileMetaData* f : inputs_[which]) {
      edit->RemoveFile(level_ + which, f->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(Slice user_key) {
  const Comparator* ucmp = vset_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const std::vector<FileMetaData*>& files = input_version_->files(lvl);
    while (level_ptrs_[lvl] < files.size()) {
      const FileMetaData* f = files[level_ptrs_[lvl]];
      if (ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // Far enough: this is the first file that ends at or after the key.
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          // The key is inside this file's range, so the file may hold it.
          return false;
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(Slice internal_key) {
  const InternalKeyComparator& icmp = vset_->icmp_;
  // Count the grandparent files the output has moved past.
  while (grandparent_index_ < grandparents_.size() &&
         icmp.Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  seen_key_ = true;

  if (overlapped_bytes_ > vset_->MaxGrandParentOverlapBytes()) {
    overlapped_bytes_ = 0;
    return true;
  }
  return false;
}

}  // namespace lsm
