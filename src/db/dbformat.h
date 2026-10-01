#pragma once

#include <cassert>
#include <cstdint>
#include <string>

#include "lsm/comparator.h"
#include "lsm/slice.h"
#include "util/coding.h"

namespace lsm {

namespace config {

constexpr int kNumLevels = 7;

// Level 0 is compacted once it has this many files.
constexpr int kL0_CompactionTrigger = 4;
// At this many level-0 files each write is delayed by 1 ms, to let
// compaction catch up without stopping writes outright.
constexpr int kL0_SlowdownWritesTrigger = 8;
// At this many level-0 files writes stop until compaction reduces them.
constexpr int kL0_StopWritesTrigger = 12;

}  // namespace config

using SequenceNumber = uint64_t;

// Sequence numbers are 56 bits: the low 8 bits of the trailer hold the type.
constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

// Values are part of the on-disk format; do not renumber.
enum ValueType : uint8_t { kTypeDeletion = 0, kTypeValue = 1 };

// Trailers sort descending, so a seek to (user_key, seq) must carry the
// highest type to land on or before every entry with that sequence.
constexpr ValueType kValueTypeForSeek = kTypeValue;

// internal_key = user_key || fixed64((sequence << 8) | type)
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
};

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  assert(seq <= kMaxSequenceNumber);
  return (seq << 8) | type;
}

void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Returns false if internal_key is too short or carries an unknown type.
bool ParseInternalKey(Slice internal_key, ParsedInternalKey* result);

// A printable form of an internal key, for error messages and debugging.
std::string DebugInternalKey(Slice internal_key);

inline Slice ExtractUserKey(Slice internal_key) {
  assert(internal_key.size() >= 8);
  return internal_key.substr(0, internal_key.size() - 8);
}

// Orders internal keys by user key ascending, then trailer descending:
// for one user key the newest sequence comes first, and at equal sequence a
// value comes before a tombstone.
class InternalKeyComparator : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* user_comparator)
      : user_comparator_(user_comparator) {}

  int Compare(Slice a, Slice b) const override;
  const char* Name() const override;
  void FindShortestSeparator(std::string* start, Slice limit) const override;
  void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const { return user_comparator_; }

 private:
  const Comparator* user_comparator_;
};

// An owned internal key. Empty until assigned; an empty InternalKey is not
// a valid key.
class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(Slice user_key, SequenceNumber seq, ValueType type) {
    AppendInternalKey(&rep_, ParsedInternalKey{user_key, seq, type});
  }

  void DecodeFrom(Slice s) { rep_.assign(s); }
  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }
  Slice user_key() const { return ExtractUserKey(rep_); }
  void Clear() { rep_.clear(); }

 private:
  std::string rep_;
};

// The key forms needed to look up a user key as of a sequence number:
//
//   varint32(internal_key_len) | user_key | fixed64 trailer
//   ^ memtable_key()           ^ internal_key() / user_key()
class LookupKey {
 public:
  LookupKey(Slice user_key, SequenceNumber sequence);
  ~LookupKey();

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  Slice memtable_key() const { return Slice(start_, end_ - start_); }
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];  // avoids a heap allocation for typical keys
};

}  // namespace lsm
