// Feeds arbitrary bytes to the write batch decoder, which recovery runs on
// every log record. It must reject malformed batches without crashing, and
// a batch it accepts must apply to a memtable.

#include <cstdint>

#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lsm;
  if (size < WriteBatchInternal::kHeader) return 0;

  WriteBatch batch;
  WriteBatchInternal::SetContents(&batch, Slice(reinterpret_cast<const char*>(data), size));
  // Keep the sequence in range, as the log's own checks would.
  WriteBatchInternal::SetSequence(&batch, WriteBatchInternal::Sequence(&batch) >> 16);

  struct Counter : WriteBatch::Handler {
    void Put(Slice, Slice) override { count++; }
    void Delete(Slice) override { count++; }
    uint32_t count = 0;
  } counter;
  if (!batch.Iterate(&counter).ok()) return 0;
  FUZZ_CHECK(counter.count == WriteBatchInternal::Count(&batch));

  MemTable* mem = new MemTable(InternalKeyComparator(BytewiseComparator()));
  mem->Ref();
  FUZZ_CHECK(WriteBatchInternal::InsertInto(&batch, mem).ok());
  mem->Unref();
  return 0;
}
