#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "lsm/status.h"

namespace lsm {

// What the database knows about one SSTable without opening it.
struct FileMetaData {
  int refs = 0;  // number of Versions that list this file
  uint64_t number = 0;
  uint64_t file_size = 0;
  InternalKey smallest;  // smallest internal key in the table
  InternalKey largest;   // largest internal key in the table
};

// A change to the set of files, and to the counters that are persisted
// with it. The manifest is a log of these; replaying them from an empty
// state reconstructs the current Version.
class VersionEdit {
 public:
  VersionEdit() = default;

  void Clear() { *this = VersionEdit(); }

  void SetComparatorName(Slice name) {
    has_comparator_ = true;
    comparator_.assign(name);
  }
  // Logs numbered below this are no longer needed for recovery.
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  // Where the next compaction of level should start.
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.emplace_back(level, key);
  }

  void AddFile(int level, uint64_t file, uint64_t file_size, const InternalKey& smallest,
               const InternalKey& largest) {
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    new_files_.emplace_back(level, f);
  }

  void RemoveFile(int level, uint64_t file) { deleted_files_.insert({level, file}); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice src);

  std::string DebugString() const;

 private:
  friend class VersionSet;

  using DeletedFileSet = std::set<std::pair<int, uint64_t>>;

  std::string comparator_;
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  DeletedFileSet deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace lsm
