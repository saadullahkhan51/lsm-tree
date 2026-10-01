#pragma once

#include "db/dbformat.h"
#include "lsm/write_batch.h"

namespace lsm {

class MemTable;

// Accessors the engine needs that do not belong in the public WriteBatch.
class WriteBatchInternal {
 public:
  // Size of the sequence and count that start every batch.
  static constexpr size_t kHeader = 12;

  static uint32_t Count(const WriteBatch* batch);
  static void SetCount(WriteBatch* batch, uint32_t n);

  // Sequence number of the first update; update i gets Sequence() + i.
  static SequenceNumber Sequence(const WriteBatch* batch);
  static void SetSequence(WriteBatch* batch, SequenceNumber seq);

  static Slice Contents(const WriteBatch* batch) { return batch->rep_; }
  static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }
  // REQUIRES: contents.size() >= kHeader.
  static void SetContents(WriteBatch* batch, Slice contents);

  static Status InsertInto(const WriteBatch* batch, MemTable* memtable);
  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace lsm
