#include "db/version_edit.h"

#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

namespace {

// Encodes, decodes, and re-encodes: the two encodings must match.
void TestEncodeDecode(const VersionEdit& edit) {
  std::string encoded, encoded2;
  edit.EncodeTo(&encoded);
  VersionEdit parsed;
  ASSERT_OK(parsed.DecodeFrom(encoded));
  parsed.EncodeTo(&encoded2);
  ASSERT_EQ(encoded, encoded2);
  ASSERT_EQ(edit.DebugString(), parsed.DebugString());
}

VersionEdit FullEdit() {
  VersionEdit edit;
  const uint64_t kBig = uint64_t{1} << 50;
  for (int i = 0; i < 4; i++) {
    edit.AddFile(3, kBig + 300 + i, kBig + 400 + i, InternalKey("foo", kBig + 500 + i, kTypeValue),
                 InternalKey("zoo", kBig + 600 + i, kTypeDeletion));
    edit.RemoveFile(4, kBig + 700 + i);
    edit.SetCompactPointer(i, InternalKey("x", kBig + 900 + i, kTypeValue));
  }
  edit.SetComparatorName("foo");
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);
  return edit;
}

}  // namespace

TEST(VersionEdit, EncodeDecode) {
  TestEncodeDecode(VersionEdit());
  TestEncodeDecode(FullEdit());

  VersionEdit binary_keys;
  binary_keys.AddFile(0, 1, 2, InternalKey(std::string("\0\xff", 2), 1, kTypeValue),
                      InternalKey("", 0, kTypeDeletion));
  TestEncodeDecode(binary_keys);
}

TEST(VersionEdit, DebugStringNamesEveryField) {
  VersionEdit edit;
  edit.SetComparatorName("cmp");
  edit.SetLogNumber(7);
  edit.AddFile(2, 9, 100, InternalKey("a", 1, kTypeValue), InternalKey("b", 2, kTypeValue));
  edit.RemoveFile(1, 5);
  const std::string debug = edit.DebugString();
  for (const char* expected : {"Comparator: cmp", "LogNumber: 7", "AddFile: 2 9 100 'a' @ 1",
                               "RemoveFile: 1 5"}) {
    ASSERT_NE(std::string::npos, debug.find(expected)) << expected << "\n" << debug;
  }
}

TEST(VersionEdit, EveryTruncationIsCorruption) {
  std::string encoded;
  FullEdit().EncodeTo(&encoded);
  // A cut can land on a field boundary and leave a shorter, valid edit.
  // Anywhere else it must be rejected, and it must never crash.
  int rejected = 0;
  for (size_t len = 0; len < encoded.size(); len++) {
    VersionEdit parsed;
    Status s = parsed.DecodeFrom(Slice(encoded.data(), len));
    ASSERT_TRUE(s.ok() || s.IsCorruption());
    if (!s.ok()) rejected++;
  }
  ASSERT_GT(rejected, static_cast<int>(encoded.size() * 3 / 4));
}

TEST(VersionEdit, RejectsBadFields) {
  auto decode = [](const std::string& bytes) {
    VersionEdit edit;
    return edit.DecodeFrom(bytes);
  };
  ASSERT_TRUE(decode("\x63").IsCorruption());          // unknown tag 99
  ASSERT_TRUE(decode("\x06\x07\x01").IsCorruption());  // deleted file at level 7
  ASSERT_TRUE(decode(std::string("\x07\x00\x01\x01\x03" "abc" "\x03" "abc", 12))
                  .IsCorruption());  // new file whose keys are shorter than a trailer

  // A last sequence beyond 56 bits cannot be a real sequence number.
  std::string big_sequence("\x04");
  big_sequence += std::string("\xff\xff\xff\xff\xff\xff\xff\xff\x7f", 9);
  ASSERT_TRUE(decode(big_sequence).IsCorruption());
}

}  // namespace lsm
