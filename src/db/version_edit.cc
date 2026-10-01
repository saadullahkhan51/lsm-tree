#include "db/version_edit.h"

#include "util/coding.h"

namespace lsm {

namespace {

// Tags for the fields of an encoded edit. Persisted in the manifest: do
// not renumber.
enum Tag : uint32_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
};

bool GetInternalKey(Slice* input, InternalKey* dst) {
  Slice str;
  // An internal key is at least its 8-byte trailer.
  if (!GetLengthPrefixedSlice(input, &str) || str.size() < 8) return false;
  dst->DecodeFrom(str);
  return true;
}

bool GetLevel(Slice* input, int* level) {
  uint32_t v;
  if (!GetVarint32(input, &v) || v >= static_cast<uint32_t>(config::kNumLevels)) return false;
  *level = static_cast<int>(v);
  return true;
}

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixedSlice(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, level);
    PutLengthPrefixedSlice(dst, key.Encode());
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, level);
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, level);
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, f.smallest.Encode());
    PutLengthPrefixedSlice(dst, f.largest.Encode());
  }
}

Status VersionEdit::DecodeFrom(Slice src) {
  Clear();
  Slice input = src;
  const char* msg = nullptr;
  uint32_t tag;

  int level;
  uint64_t number;
  FileMetaData f;
  Slice str;
  InternalKey key;

  while (msg == nullptr && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kComparator:
        if (GetLengthPrefixedSlice(&input, &str)) {
          comparator_.assign(str);
          has_comparator_ = true;
        } else {
          msg = "comparator name";
        }
        break;

      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          msg = "log number";
        }
        break;

      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          msg = "next file number";
        }
        break;

      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_) && last_sequence_ <= kMaxSequenceNumber) {
          has_last_sequence_ = true;
        } else {
          msg = "last sequence number";
        }
        break;

      case kCompactPointer:
        if (GetLevel(&input, &level) && GetInternalKey(&input, &key)) {
          compact_pointers_.emplace_back(level, key);
        } else {
          msg = "compaction pointer";
        }
        break;

      case kDeletedFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
          deleted_files_.insert({level, number});
        } else {
          msg = "deleted file";
        }
        break;

      case kNewFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &f.number) &&
            GetVarint64(&input, &f.file_size) && GetInternalKey(&input, &f.smallest) &&
            GetInternalKey(&input, &f.largest)) {
          new_files_.emplace_back(level, f);
        } else {
          msg = "new-file entry";
        }
        break;

      default:
        msg = "unknown tag";
        break;
    }
  }

  if (msg == nullptr && !input.empty()) {
    msg = "invalid tag";
  }
  if (msg != nullptr) {
    return Status::Corruption("VersionEdit", msg);
  }
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string r = "VersionEdit {";
  if (has_comparator_) r += "\n  Comparator: " + comparator_;
  if (has_log_number_) r += "\n  LogNumber: " + std::to_string(log_number_);
  if (has_next_file_number_) r += "\n  NextFile: " + std::to_string(next_file_number_);
  if (has_last_sequence_) r += "\n  LastSeq: " + std::to_string(last_sequence_);
  for (const auto& [level, key] : compact_pointers_) {
    r += "\n  CompactPointer: " + std::to_string(level) + " " + DebugInternalKey(key.Encode());
  }
  for (const auto& [level, number] : deleted_files_) {
    r += "\n  RemoveFile: " + std::to_string(level) + " " + std::to_string(number);
  }
  for (const auto& [level, f] : new_files_) {
    r += "\n  AddFile: " + std::to_string(level) + " " + std::to_string(f.number) + " " +
         std::to_string(f.file_size) + " " + DebugInternalKey(f.smallest.Encode()) + " .. " +
         DebugInternalKey(f.largest.Encode());
  }
  r += "\n}\n";
  return r;
}

}  // namespace lsm
