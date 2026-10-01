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

  void Ref();
  void Unref();

  int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }
  const std::vector<FileMetaData*>& files(int level) const { return files_[level]; }

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
};

// Owns the chain of live Versions, the manifest, and the counters that are
// persisted in it. All methods require the database mutex unless noted.
class VersionSet {
 public:
  VersionSet(const std::string& dbname, Env* env, TableCache* table_cache,
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
  // If this fails, the manifest may end in a partial record. The caller
  // must not call LogAndApply again.
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

 private:
  class Builder;
  friend class Version;

  // Writes the whole current state as a single edit.
  Status WriteSnapshot(log::Writer* log);
  void AppendVersion(Version* v);

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

  Version dummy_versions_;  // head of the circular list
  Version* current_ = nullptr;  // == dummy_versions_.prev_

  // Per-level key at which the next compaction of that level starts.
  // Empty, or a valid InternalKey encoding.
  std::string compact_pointer_[config::kNumLevels];
};

}  // namespace lsm
