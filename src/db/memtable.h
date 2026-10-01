#pragma once

#include <string>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "lsm/iterator.h"
#include "lsm/status.h"
#include "util/arena.h"

namespace lsm {

// The in-memory write buffer: a skiplist of entries ordered by internal key.
//
// Thread safety: Add needs one writer at a time (the group-commit leader).
// Get takes no locks and may run concurrently with Add. Ref and Unref are
// protected by the DB mutex.
class MemTable {
 public:
  // Starts with zero references; the caller must Ref() it.
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Ref() { ++refs_; }
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) delete this;
  }

  // Arena bytes in use. Safe to call while the memtable is being modified.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  // For the writer only: not synchronized with Add.
  bool empty() const { return num_entries_ == 0; }

  // Adds an entry. For kTypeDeletion, value is ignored and should be empty.
  void Add(SequenceNumber seq, ValueType type, Slice key, Slice value);

  // Looks up the newest entry for key.user_key() at or below key's sequence.
  // If it is a value, stores it in *value and returns true. If it is a
  // tombstone, sets *s to NotFound and returns true. Otherwise returns false.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

  // An iterator over the entries, yielding internal keys. The caller must
  // keep a reference on the memtable for as long as the iterator is live.
  // Like Get, it may be used while a writer is adding entries.
  Iterator* NewIterator() const;

 private:
  friend class MemTableIterator;

  ~MemTable() { assert(refs_ == 0); }  // deleted only through Unref

  // Compares two entries by the internal key each one starts with.
  struct KeyComparator {
    const InternalKeyComparator comparator;
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  KeyComparator comparator_;
  int refs_ = 0;
  size_t num_entries_ = 0;
  Arena arena_;
  Table table_;
};

}  // namespace lsm
