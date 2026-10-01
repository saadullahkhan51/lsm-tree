#include "lsm/write_batch.h"

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace lsm {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeader);
}

void WriteBatch::Put(Slice key, Slice value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(Slice key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) { WriteBatchInternal::Append(this, &source); }

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < WriteBatchInternal::kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(WriteBatchInternal::kHeader);

  Slice key, value;
  uint32_t found = 0;
  while (!input.empty()) {
    found++;
    const char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (!GetLengthPrefixedSlice(&input, &key) || !GetLengthPrefixedSlice(&input, &value)) {
          return Status::Corruption("bad WriteBatch Put");
        }
        handler->Put(key, value);
        break;
      case kTypeDeletion:
        if (!GetLengthPrefixedSlice(&input, &key)) {
          return Status::Corruption("bad WriteBatch Delete");
        }
        handler->Delete(key);
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  }
  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch* batch) {
  return DecodeFixed32(batch->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* batch, uint32_t n) {
  EncodeFixed32(batch->rep_.data() + 8, n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* batch) {
  return DecodeFixed64(batch->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* batch, SequenceNumber seq) {
  EncodeFixed64(batch->rep_.data(), seq);
}

void WriteBatchInternal::SetContents(WriteBatch* batch, Slice contents) {
  assert(contents.size() >= kHeader);
  batch->rep_.assign(contents);
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_, kHeader);
}

namespace {

// Applies a batch to a memtable, numbering its updates consecutively.
class MemTableInserter : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber sequence, MemTable* mem) : sequence_(sequence), mem_(mem) {}

  void Put(Slice key, Slice value) override { mem_->Add(sequence_++, kTypeValue, key, value); }
  void Delete(Slice key) override { mem_->Add(sequence_++, kTypeDeletion, key, Slice()); }

 private:
  SequenceNumber sequence_;
  MemTable* mem_;
};

}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* batch, MemTable* memtable) {
  MemTableInserter inserter(Sequence(batch), memtable);
  return batch->Iterate(&inserter);
}

}  // namespace lsm
