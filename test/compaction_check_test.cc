#include "db/compaction_check.h"

#include <memory>
#include <string>
#include <vector>

#include "db/memtable.h"
#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

namespace {

struct Entry {
  std::string user_key;
  SequenceNumber sequence;
  ValueType type;
  std::string value;
};

Entry Put(const std::string& key, SequenceNumber seq, const std::string& value) {
  return {key, seq, kTypeValue, value};
}
Entry Del(const std::string& key, SequenceNumber seq) { return {key, seq, kTypeDeletion, ""}; }

// A set of entries behind an Iterator, as a compaction's input or output.
class EntrySet {
 public:
  explicit EntrySet(const std::vector<Entry>& entries)
      : mem_(new MemTable(InternalKeyComparator(BytewiseComparator()))) {
    mem_->Ref();
    for (const Entry& e : entries) mem_->Add(e.sequence, e.type, e.user_key, e.value);
  }
  ~EntrySet() { mem_->Unref(); }
  Iterator* NewIterator() const { return mem_->NewIterator(); }

 private:
  MemTable* mem_;
};

const SequenceNumber kLatest = kMaxSequenceNumber;

Status Check(const std::vector<Entry>& input, const std::vector<Entry>& output,
             const std::vector<SequenceNumber>& read_points, bool key_may_exist_below = false) {
  const EntrySet input_set(input), output_set(output);
  std::unique_ptr<Iterator> in(input_set.NewIterator());
  std::unique_ptr<Iterator> out(output_set.NewIterator());
  return CheckCompactionOutput(BytewiseComparator(), in.get(), out.get(), read_points,
                               [=](Slice) { return key_may_exist_below; });
}

}  // namespace

TEST(CompactionCheck, IdenticalOutputPasses) {
  const std::vector<Entry> entries = {Put("a", 5, "a5"), Put("a", 3, "a3"), Del("b", 4),
                                      Put("c", 1, "c1")};
  ASSERT_OK(Check(entries, entries, {kLatest}));
  ASSERT_OK(Check({}, {}, {kLatest}));
}

TEST(CompactionCheck, DroppingShadowedVersionsPasses) {
  // Only the newest version of "a" is visible to a reader of the latest
  // state.
  ASSERT_OK(Check({Put("a", 5, "a5"), Put("a", 3, "a3"), Put("a", 1, "a1"), Put("b", 2, "b2")},
                  {Put("a", 5, "a5"), Put("b", 2, "b2")}, {kLatest}));
}

TEST(CompactionCheck, DroppingAVersionASnapshotSeesFails) {
  const std::vector<Entry> input = {Put("a", 5, "a5"), Put("a", 3, "a3")};
  // A snapshot at sequence 4 reads "a3".
  Status s = Check(input, {Put("a", 5, "a5")}, {4, kLatest});
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_NE(std::string::npos, s.ToString().find("a visible value was dropped")) << s.ToString();
  // With the version kept, it passes.
  ASSERT_OK(Check(input, input, {4, kLatest}));
}

TEST(CompactionCheck, DroppingTheNewestValueFails) {
  Status s = Check({Put("a", 5, "a5"), Put("b", 2, "b2")}, {Put("b", 2, "b2")}, {kLatest});
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_NE(std::string::npos, s.ToString().find("'a'")) << s.ToString();
}

TEST(CompactionCheck, DroppingANewerVersionExposesAnOlderOne) {
  // The reader used to see a5 and would now see a3.
  Status s = Check({Put("a", 5, "a5"), Put("a", 3, "a3")}, {Put("a", 3, "a3")}, {kLatest});
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_NE(std::string::npos, s.ToString().find("a different entry is visible")) << s.ToString();
}

TEST(CompactionCheck, TombstoneMayGoOnlyIfNothingBelowCanResurface) {
  const std::vector<Entry> input = {Del("a", 5), Put("a", 3, "a3")};

  // Bottom of the tree: the tombstone and what it hid both go.
  ASSERT_OK(Check(input, {}, {kLatest}, /*key_may_exist_below=*/false));

  // A lower level may still hold "a": without the tombstone that older
  // value would come back.
  Status s = Check(input, {}, {kLatest}, /*key_may_exist_below=*/true);
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_NE(std::string::npos, s.ToString().find("a tombstone was dropped")) << s.ToString();

  // Keeping the tombstone is always fine.
  ASSERT_OK(Check(input, {Del("a", 5)}, {kLatest}, /*key_may_exist_below=*/true));
}

TEST(CompactionCheck, DroppingATombstoneButKeepingWhatItHidFails) {
  // The classic resurrection, inside one compaction.
  Status s = Check({Del("a", 5), Put("a", 3, "a3")}, {Put("a", 3, "a3")}, {kLatest});
  ASSERT_TRUE(s.IsCorruption());
}

TEST(CompactionCheck, TombstoneASnapshotCannotSeeYetMustNotHideItsValue) {
  // A snapshot at 4 still reads a3; the tombstone at 5 is newer than it.
  const std::vector<Entry> input = {Del("a", 5), Put("a", 3, "a3")};
  ASSERT_TRUE(Check(input, {}, {4, kLatest}).IsCorruption());
  ASSERT_TRUE(Check(input, {Del("a", 5)}, {4, kLatest}).IsCorruption());
  ASSERT_OK(Check(input, input, {4, kLatest}));
}

TEST(CompactionCheck, OutputMustNotInventOrAlterEntries) {
  const std::vector<Entry> input = {Put("a", 5, "a5"), Put("c", 2, "c2")};

  // A key the input does not have, before, between, and after its keys.
  ASSERT_TRUE(Check(input, {Put("A", 1, "x"), Put("a", 5, "a5"), Put("c", 2, "c2")}, {kLatest})
                  .IsCorruption());
  ASSERT_TRUE(Check(input, {Put("a", 5, "a5"), Put("b", 1, "x"), Put("c", 2, "c2")}, {kLatest})
                  .IsCorruption());
  ASSERT_TRUE(Check(input, {Put("a", 5, "a5"), Put("c", 2, "c2"), Put("d", 1, "x")}, {kLatest})
                  .IsCorruption());

  // A changed value, a changed sequence, a changed type.
  ASSERT_TRUE(Check(input, {Put("a", 5, "XX"), Put("c", 2, "c2")}, {kLatest}).IsCorruption());
  ASSERT_TRUE(Check(input, {Put("a", 6, "a5"), Put("c", 2, "c2")}, {kLatest}).IsCorruption());
  ASSERT_TRUE(Check(input, {Del("a", 5), Put("c", 2, "c2")}, {kLatest}).IsCorruption());
}

}  // namespace lsm
