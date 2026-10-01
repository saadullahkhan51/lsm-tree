#include "db/memtable.h"

#include <cstring>

#include "util/coding.h"

namespace lsm {

// Entry layout, one contiguous arena allocation:
//   varint32(internal_key_len) | user_key | fixed64 trailer |
//   varint32(value_len) | value

namespace {

// Entries are written by Add and never truncated, so the varint is known to
// fit; +5 is the longest a varint32 can be.
Slice GetLengthPrefixed(const char* data) {
  uint32_t len;
  const char* p = GetVarint32Ptr(data, data + 5, &len);
  return Slice(p, len);
}

}  // namespace

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(const MemTable::Table* table) : iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }
  void Seek(Slice target) override {
    // The skiplist holds length-prefixed keys, so the target needs the
    // same prefix to be comparable.
    tmp_.clear();
    PutLengthPrefixedSlice(&tmp_, target);
    iter_.Seek(tmp_.data());
  }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }
  Slice key() const override { return GetLengthPrefixed(iter_.key()); }
  Slice value() const override {
    const Slice key = GetLengthPrefixed(iter_.key());
    return GetLengthPrefixed(key.data() + key.size());
  }
  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;  // scratch for Seek
};

Iterator* MemTable::NewIterator() const { return new MemTableIterator(&table_); }

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_{comparator}, table_(comparator_, &arena_) {}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator.Compare(GetLengthPrefixed(a), GetLengthPrefixed(b));
}

void MemTable::Add(SequenceNumber seq, ValueType type, Slice key, Slice value) {
  const size_t internal_key_size = key.size() + 8;
  const size_t encoded_len = VarintLength(internal_key_size) + internal_key_size +
                             VarintLength(value.size()) + value.size();
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  std::memcpy(p, value.data(), value.size());
  assert(p + value.size() == buf + encoded_len);
  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  Table::Iterator iter(&table_);
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) return false;

  // The seek landed on the first entry at or after (user_key, sequence).
  // The comparator ordered it by user key and then sequence descending, so
  // if the user key matches, this is the newest version visible at that
  // sequence.
  const Slice internal_key = GetLengthPrefixed(iter.key());
  if (comparator_.comparator.user_comparator()->Compare(ExtractUserKey(internal_key),
                                                        key.user_key()) != 0) {
    return false;
  }
  const uint64_t trailer = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  switch (static_cast<ValueType>(trailer & 0xff)) {
    case kTypeValue:
      value->assign(GetLengthPrefixed(internal_key.data() + internal_key.size()));
      return true;
    case kTypeDeletion:
      *s = Status::NotFound({});
      return true;
  }
  return false;
}

}  // namespace lsm
