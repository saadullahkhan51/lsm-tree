#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/version_edit.h"
#include "lsm/env.h"
#include "lsm/options.h"

namespace lsm {

class Compaction;
class Iterator;
class TableCache;
class VersionSet;

// Index of the first file in files whose largest key is >= key, or
// files.size() if there is none. REQUIRES: files is sorted and its ranges
// do not overlap (any level but 0).
size_t FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files,
                Slice key);

// An immutable snapshot of which files make up each level.
//
// Versions are reference counted. A reader pins the current Version for the
// length of its read; installing a new Version (after a flush or a
// compaction) does not disturb it, and the files it needs are not deleted
// until it lets go. Ref and Unref require the database mutex.
class Version {
 public:
  // How much work a lookup did, for the read amplification metric.
  struct GetStats {
    int files_probed = 0;
  };

  // Looks up key in the files of this version, newest data first. Returns
  // OK with *value filled in, NotFound, or the error that stopped the
  // search. Does not need the database mutex.
  Status Get(const ReadOptions& options, const LookupKey& key, std::string* value,
             GetStats* stats) const;

  // Appends to *iters iterators that together yield every entry in this
  // version when merged: one per level-0 file, one per deeper level. The
  // version must stay referenced while they are live.
  void AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) const;

  void Ref();
  void Unref();

  int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }
  const std::vector<FileMetaData*>& files(int level) const { return files_[level]; }

  // Stores in *inputs every file in level that overlaps the user-key range
  // [begin, end]. A null bound is open. In level 0, where files overlap one
  // another, the range grows to cover each file found, so the result is
  // closed under overlap.
  void GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end,
                            std::vector<FileMetaData*>* inputs) const;

  // True if some file in level overlaps the user-key range. Null bounds are
  // open.
  bool OverlapInLevel(int level, const Slice* smallest_user_key,
                      const Slice* largest_user_key) const;

  // Verifies the level structure from the metadata alone:
  //   - every file has smallest <= largest;
  //   - level 0 is ordered newest file first;
  //   - in levels 1 and up, files are sorted and no two share a user key;
  //   - no file number appears twice.
  // Returns Corruption describing the first violation.
  Status CheckInvariants() const;

  std::string DebugString() const;

 private:
  friend class VersionSet;

  explicit Version(VersionSet* vset) : vset_(vset), next_(this), prev_(this) {}
  ~Version();

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  VersionSet* vset_;
  Version* next_;  // doubly-linked list of the versions still alive
  Version* prev_;
  int refs_ = 0;

  // Level 0: newest file first (ranges may overlap).
  // Levels 1+: sorted by key, ranges disjoint.
  std::vector<FileMetaData*> files_[config::kNumLevels];

  // The level most in need of compaction and how badly: a score >= 1 means
  // it is over its target. Filled in when the version is installed.
  double compaction_score_ = -1;
  int compaction_level_ = -1;
};

// Owns the chain of live Versions, the manifest, and the counters that are
// persisted in it. All methods require the database mutex unless noted.
class VersionSet {
 public:
  VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache,
             const InternalKeyComparator* icmp);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Applies *edit to the current version, appends it to the manifest, and
  // makes the result current. The edit is durable when this returns OK.
  //
  // lock is held on entry and exit and released while the manifest is
  // written. REQUIRES: no other thread is in LogAndApply.
  //
  // If a manifest write fails, the manifest may end in a partial record,
  // and anything appended after it would be unreadable. From then on every
  // call fails with the same error without touching the file; the database
  // has to be reopened.
  Status LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock);

  // Writes the manifest and CURRENT for a database that has no files yet.
  static Status CreateNew(const std::string& dbname, Env* env, const Comparator* user_comparator);

  // Rebuilds the current version by replaying the manifest CURRENT names.
  Status Recover();

  Version* current() const { return current_; }

  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  uint64_t NewFileNumber() { return next_file_number_++; }

  // Makes sure number is never handed out by NewFileNumber.
  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) next_file_number_ = number + 1;
  }

  int NumLevelFiles(int level) const { return current_->NumFiles(level); }
  uint64_t NumLevelBytes(int level) const;

  // Sequence number of the newest write visible to readers.
  SequenceNumber LastSequence() const { return last_sequence_; }
  void SetLastSequence(SequenceNumber s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
  }

  // Logs numbered below this are not needed for recovery.
  uint64_t LogNumber() const { return log_number_; }

  // Adds the number of every file in every live version.
  void AddLiveFiles(std::set<uint64_t>* live) const;

  // True if some level is over its target.
  bool NeedsCompaction() const { return current_->compaction_score_ >= 1; }

  // Chooses the next compaction: the level furthest over its target, and
  // within it the file after the level's compaction pointer. Returns null
  // if nothing needs compacting. The caller owns the result.
  Compaction* PickCompaction();

  // A compaction of the files in level that overlap [begin, end], or null
  // if there are none. Null bounds are open. The caller owns the result.
  Compaction* CompactRange(int level, const InternalKey* begin, const InternalKey* end);

  // An iterator over all the entries in the compaction's inputs, merged.
  // The caller owns it.
  Iterator* MakeInputIterator(Compaction* c);

  // An iterator over the entries of a sorted, disjoint run of files, which
  // must outlive it. The caller owns it.
  Iterator* NewConcatenatingIterator(const ReadOptions& options,
                                     const std::vector<FileMetaData*>* files) const;

  uint64_t MaxBytesForLevel(int level) const;
  // A compaction output is cut short once it overlaps this many bytes in
  // the level below its own, so that compacting it later stays bounded.
  uint64_t MaxGrandParentOverlapBytes() const { return 10 * uint64_t{options_->max_file_size}; }

  const InternalKeyComparator& icmp() const { return icmp_; }

 private:
  class Builder;
  friend class Compaction;
  friend class Version;

  // Computes v's compaction score.
  void Finalize(Version* v) const;
  void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                InternalKey* largest) const;
  void GetRange2(const std::vector<FileMetaData*>& inputs1,
                 const std::vector<FileMetaData*>& inputs2, InternalKey* smallest,
                 InternalKey* largest) const;
  // Fills in the level+1 inputs and the grandparents, and advances the
  // level's compaction pointer.
  void SetupOtherInputs(Compaction* c);

  // Writes the whole current state as a single edit.
  Status WriteSnapshot(log::Writer* log);
  void AppendVersion(Version* v);

  const Options* const options_;
  Env* const env_;
  const std::string dbname_;
  TableCache* const table_cache_;
  const InternalKeyComparator icmp_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  uint64_t log_number_ = 0;

  // The open manifest. Null until the first LogAndApply, which starts a new
  // one.
  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<log::Writer> descriptor_log_;
  // The first error from writing the manifest. Sticky.
  Status manifest_error_;

  Version dummy_versions_;  // head of the circular list
  Version* current_ = nullptr;  // == dummy_versions_.prev_

  // Per-level key at which the next compaction of that level starts.
  // Empty, or a valid InternalKey encoding.
  std::string compact_pointer_[config::kNumLevels];
};

// The inputs and bookkeeping of one compaction: files from `level` are
// merged with the files they overlap in `level + 1`, and the result
// replaces both in `level + 1`.
class Compaction {
 public:
  ~Compaction();

  int level() const { return level_; }

  // The edit that will install this compaction's result.
  VersionEdit* edit() { return &edit_; }

  // which is 0 for the inputs from level(), 1 for those from level() + 1.
  int num_input_files(int which) const { return static_cast<int>(inputs_[which].size()); }
  FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // True if the compaction can be done by moving its single input file down
  // a level, with no merging and no rewriting.
  bool IsTrivialMove() const;

  // Records the removal of every input file in *edit.
  void AddInputDeletions(VersionEdit* edit);

  // True if no level below level() + 1 holds user_key. A tombstone for the
  // key may then be dropped: nothing remains for it to hide. Must be called
  // with keys in increasing order.
  bool IsBaseLevelForKey(Slice user_key);

  // True if the output file being built should be finished before
  // internal_key is added, because it already overlaps too much of the
  // level below. Must be called with every key, in order.
  bool ShouldStopBefore(Slice internal_key);

  // Releases the input version. Call once the compaction is done with it.
  void ReleaseInputs();

  // The version the inputs were chosen from.
  const Version* input_version() const { return input_version_; }

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const VersionSet* vset, int level);

  const VersionSet* const vset_;
  const int level_;
  const uint64_t max_output_file_size_;
  Version* input_version_ = nullptr;
  VersionEdit edit_;

  std::vector<FileMetaData*> inputs_[2];

  // Files in level + 2 that overlap the compaction; used to limit how much
  // of that level a single output file may overlap.
  std::vector<FileMetaData*> grandparents_;
  size_t grandparent_index_ = 0;  // position in grandparents_
  bool seen_key_ = false;         // some output key has been seen
  uint64_t overlapped_bytes_ = 0; // grandparent bytes the current output overlaps

  // IsBaseLevelForKey state: for each level below level + 1, the index of
  // the first file that could still contain a key at or after the last one
  // asked about.
  size_t level_ptrs_[config::kNumLevels] = {};
};

}  // namespace lsm
