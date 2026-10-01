#include "lsm/write_batch.h"

#include <string>

#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

namespace {

// Renders a batch as "Put(k, v)@seq" / "Delete(k)@seq" entries in order.
class Printer : public WriteBatch::Handler {
 public:
  explicit Printer(SequenceNumber sequence) : sequence_(sequence) {}

  void Put(Slice key, Slice value) override {
    out_ += "Put(" + std::string(key) + ", " + std::string(value) + ")@" +
            std::to_string(sequence_++);
  }
  void Delete(Slice key) override {
    out_ += "Delete(" + std::string(key) + ")@" + std::to_string(sequence_++);
  }

  std::string out_;

 private:
  SequenceNumber sequence_;
};

std::string Print(const WriteBatch& batch) {
  Printer printer(WriteBatchInternal::Sequence(&batch));
  Status s = batch.Iterate(&printer);
  return s.ok() ? printer.out_ : printer.out_ + "<" + s.ToString() + ">";
}

}  // namespace

TEST(WriteBatch, Empty) {
  WriteBatch batch;
  ASSERT_EQ("", Print(batch));
  ASSERT_EQ(0u, WriteBatchInternal::Count(&batch));
  ASSERT_EQ(WriteBatchInternal::kHeader, batch.ApproximateSize());
}

TEST(WriteBatch, UpdatesKeepTheirOrderAndGetConsecutiveSequences) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  batch.Put("baz", "boo");
  WriteBatchInternal::SetSequence(&batch, 100);
  ASSERT_EQ(100u, WriteBatchInternal::Sequence(&batch));
  ASSERT_EQ(3u, WriteBatchInternal::Count(&batch));
  ASSERT_EQ("Put(foo, bar)@100Delete(box)@101Put(baz, boo)@102", Print(batch));
}

TEST(WriteBatch, EncodedLayout) {
  WriteBatch batch;
  batch.Put("k", "v");
  batch.Delete("d");
  WriteBatchInternal::SetSequence(&batch, 0x0102);
  const std::string expected("\x02\x01\x00\x00\x00\x00\x00\x00"  // sequence
                             "\x02\x00\x00\x00"                  // count
                             "\x01\x01k\x01v"                    // kTypeValue
                             "\x00\x01"
                             "d",  // kTypeDeletion
                             20);
  ASSERT_EQ(expected, WriteBatchInternal::Contents(&batch));
}

TEST(WriteBatch, InsertIntoMemTable) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  WriteBatchInternal::SetSequence(&batch, 10);

  MemTable* mem = new MemTable(InternalKeyComparator(BytewiseComparator()));
  mem->Ref();
  ASSERT_OK(WriteBatchInternal::InsertInto(&batch, mem));

  std::string value;
  Status s;
  // At sequence 10 only the first Put has happened.
  ASSERT_TRUE(mem->Get(LookupKey("a", 10), &value, &s));
  ASSERT_EQ("1", value);
  ASSERT_FALSE(mem->Get(LookupKey("b", 10), &value, &s));
  // The Delete of "a" took sequence 12, later in the same batch.
  ASSERT_TRUE(mem->Get(LookupKey("a", 12), &value, &s));
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_TRUE(mem->Get(LookupKey("b", 12), &value, &s));
  ASSERT_EQ("2", value);
  mem->Unref();
}

TEST(WriteBatch, TruncatedContentsAreCorruption) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  WriteBatchInternal::SetSequence(&batch, 200);
  const std::string contents(WriteBatchInternal::Contents(&batch));

  // Every cut that removes part of a record must be reported.
  for (size_t len = WriteBatchInternal::kHeader; len < contents.size(); len++) {
    WriteBatch cut;
    WriteBatchInternal::SetContents(&cut, Slice(contents.data(), len));
    Printer printer(200);
    ASSERT_TRUE(cut.Iterate(&printer).IsCorruption()) << "len=" << len;
  }
}

TEST(WriteBatch, UnknownTagIsCorruption) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  std::string contents(WriteBatchInternal::Contents(&batch));
  contents[WriteBatchInternal::kHeader] = 7;
  WriteBatchInternal::SetContents(&batch, contents);
  Printer printer(0);
  ASSERT_TRUE(batch.Iterate(&printer).IsCorruption());
}

TEST(WriteBatch, WrongCountIsCorruption) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  WriteBatchInternal::SetCount(&batch, 2);
  Printer printer(0);
  ASSERT_TRUE(batch.Iterate(&printer).IsCorruption());
}

TEST(WriteBatch, Append) {
  WriteBatch b1, b2;
  WriteBatchInternal::SetSequence(&b1, 200);
  WriteBatchInternal::SetSequence(&b2, 300);
  b1.Append(b2);
  ASSERT_EQ("", Print(b1));

  b2.Put("a", "va");
  b1.Append(b2);
  ASSERT_EQ("Put(a, va)@200", Print(b1));

  b2.Clear();
  b2.Put("b", "vb");
  b1.Append(b2);
  ASSERT_EQ("Put(a, va)@200Put(b, vb)@201", Print(b1));

  b2.Delete("foo");
  b1.Append(b2);
  ASSERT_EQ("Put(a, va)@200Put(b, vb)@201Put(b, vb)@202Delete(foo)@203", Print(b1));
  ASSERT_EQ(4u, WriteBatchInternal::Count(&b1));
}

TEST(WriteBatch, ApproximateSizeGrows) {
  WriteBatch batch;
  const size_t empty_size = batch.ApproximateSize();
  batch.Put("foo", "bar");
  const size_t one_key_size = batch.ApproximateSize();
  ASSERT_LT(empty_size, one_key_size);
  batch.Put("baz", "boo");
  const size_t two_keys_size = batch.ApproximateSize();
  ASSERT_LT(one_key_size, two_keys_size);
  batch.Delete("box");
  ASSERT_LT(two_keys_size, batch.ApproximateSize());
  batch.Clear();
  ASSERT_EQ(empty_size, batch.ApproximateSize());
}

}  // namespace lsm
