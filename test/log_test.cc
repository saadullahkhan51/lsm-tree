#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/log_reader.h"
#include "db/log_writer.h"
#include "gtest/gtest.h"
#include "testutil.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm::log {

namespace {

// A string of length n made by repeating partial_string.
std::string BigString(const std::string& partial_string, size_t n) {
  std::string result;
  while (result.size() < n) result.append(partial_string);
  result.resize(n);
  return result;
}

std::string NumberString(int n) { return std::to_string(n) + "."; }

// A record whose content and length both depend on i.
std::string RandomSkewedLengthString(int i, std::mt19937* rnd) {
  const int max_log = 17;
  const size_t length = (*rnd)() % (size_t{1} << ((*rnd)() % (max_log + 1)));
  return BigString(NumberString(i), length);
}

class StringDest : public WritableFile {
 public:
  Status Append(Slice data) override {
    contents_.append(data);
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  std::string contents_;
};

class StringSource : public SequentialFile {
 public:
  explicit StringSource(Slice contents) : contents_(contents) {}

  Status Read(size_t n, Slice* result, char* scratch) override {
    if (fail_reads_) return Status::IOError("injected read error");
    n = std::min(n, contents_.size());
    std::memcpy(scratch, contents_.data(), n);
    *result = Slice(scratch, n);
    contents_.remove_prefix(n);
    return Status::OK();
  }
  Status Skip(uint64_t n) override {
    contents_.remove_prefix(std::min<uint64_t>(n, contents_.size()));
    return Status::OK();
  }

  bool fail_reads_ = false;

 private:
  Slice contents_;
};

// What reading a whole log produced.
struct ReadResult {
  std::vector<std::string> records;
  Status status;
  bool tail_truncated = false;
};

ReadResult ReadAll(Slice contents) {
  StringSource source(contents);
  Reader reader(&source);
  ReadResult result;
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    result.records.emplace_back(record);
  }
  result.status = reader.status();
  result.tail_truncated = reader.tail_truncated();
  return result;
}

bool IsPrefixOf(const std::vector<std::string>& prefix, const std::vector<std::string>& all) {
  return prefix.size() <= all.size() && std::equal(prefix.begin(), prefix.end(), all.begin());
}

class LogTest : public testing::Test {
 protected:
  void Write(const std::string& record) {
    ASSERT_OK(writer_.AddRecord(record));
    written_.push_back(record);
  }

  std::string& contents() { return dest_.contents_; }

  // Recomputes the CRC of the physical record whose header is at offset,
  // so that a test can alter a header field without tripping the checksum.
  void FixChecksum(size_t header_offset, size_t payload_length) {
    const uint32_t crc = crc32c::Value(&contents()[header_offset + 6], 1 + payload_length);
    EncodeFixed32(&contents()[header_offset], crc32c::Mask(crc));
  }

  StringDest dest_;
  Writer writer_{&dest_};
  std::vector<std::string> written_;
};

}  // namespace

TEST_F(LogTest, Empty) {
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_OK(r.status);
  ASSERT_FALSE(r.tail_truncated);
}

TEST_F(LogTest, ReadWrite) {
  Write("foo");
  Write("bar");
  Write("");
  Write("xxxx");
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
  ASSERT_FALSE(r.tail_truncated);
}

TEST_F(LogTest, PhysicalLayoutOfOneRecord) {
  Write("abc");
  ASSERT_EQ(static_cast<size_t>(kHeaderSize) + 3, contents().size());
  ASSERT_EQ(3, contents()[4]);  // length, low byte
  ASSERT_EQ(0, contents()[5]);  // length, high byte
  ASSERT_EQ(kFullType, contents()[6]);
  ASSERT_EQ("abc", contents().substr(kHeaderSize));
  // The CRC covers the type byte and the payload, and is stored masked.
  ASSERT_EQ(crc32c::Mask(crc32c::Value("\x01" "abc", 4)), DecodeFixed32(contents().data()));
}

TEST_F(LogTest, ManyBlocks) {
  for (int i = 0; i < 100000; i++) Write(NumberString(i));
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
}

TEST_F(LogTest, Fragmentation) {
  Write("small");
  Write(BigString("medium", 50000));   // spans two blocks
  Write(BigString("large", 100000));  // spans four
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
}

TEST_F(LogTest, MarginalTrailer) {
  // Leave exactly a header's worth of space at the end of the first block.
  const int n = kBlockSize - 2 * kHeaderSize;
  Write(BigString("foo", n));
  ASSERT_EQ(static_cast<size_t>(kBlockSize - kHeaderSize), contents().size());
  Write("");  // fits: a header and no payload
  Write("bar");
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
}

TEST_F(LogTest, MarginalTrailerFragmentsTheNextRecord) {
  // With exactly a header's worth left, a non-empty record gets an empty
  // FIRST fragment there and the payload in the next block.
  const int n = kBlockSize - 2 * kHeaderSize;
  Write(BigString("foo", n));
  Write("bar");
  ASSERT_EQ(static_cast<size_t>(kBlockSize + kHeaderSize + 3), contents().size());
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
}

TEST_F(LogTest, ShortTrailerIsZeroFilled) {
  // Leave 3 bytes: too small for a header, so they are padding.
  const int n = kBlockSize - 2 * kHeaderSize + 4;
  Write(BigString("foo", n));
  ASSERT_EQ(static_cast<size_t>(kBlockSize - kHeaderSize + 4), contents().size());
  Write("");
  Write("bar");
  ASSERT_EQ(std::string(3, '\0'), contents().substr(kBlockSize - 3, 3));
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
}

TEST_F(LogTest, FileEndingAtOrInsideBlockTrailer) {
  const int n = kBlockSize - 2 * kHeaderSize + 4;
  Write(BigString("foo", n));
  ASSERT_EQ(static_cast<size_t>(kBlockSize - 3), contents().size());
  // No padding written yet, then padding cut short, then all of it.
  for (int padding = 0; padding <= 3; padding++) {
    const ReadResult r = ReadAll(contents() + std::string(padding, '\0'));
    ASSERT_EQ(written_, r.records) << padding;
    ASSERT_OK(r.status);
  }
}

TEST_F(LogTest, RandomSizes) {
  std::mt19937 rnd(301);
  for (int i = 0; i < 500; i++) Write(RandomSkewedLengthString(i, &rnd));
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
}

TEST_F(LogTest, ReadErrorIsReported) {
  Write("foo");
  StringSource source(contents());
  source.fail_reads_ = true;
  Reader reader(&source);
  Slice record;
  std::string scratch;
  ASSERT_FALSE(reader.ReadRecord(&record, &scratch));
  ASSERT_TRUE(reader.status().IsIOError());
}

// --- Truncated tails: what a crash in the middle of an append leaves. ---

TEST_F(LogTest, TruncatedHeaderIsCleanEnd) {
  Write("foo");
  Write("bar");
  contents().resize(contents().size() - 3 - 4);  // cut into the second header
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_OK(r.status);
  ASSERT_TRUE(r.tail_truncated);
}

TEST_F(LogTest, TruncatedPayloadIsCleanEnd) {
  Write("foo");
  Write("barbarbar");
  contents().resize(contents().size() - 1);
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_OK(r.status);
  ASSERT_TRUE(r.tail_truncated);
}

TEST_F(LogTest, MissingLastFragmentIsCleanEnd) {
  Write("foo");
  Write(BigString("bar", kBlockSize));  // FIRST in block 0, LAST in block 1
  contents().resize(kBlockSize);        // the LAST fragment never made it
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_OK(r.status);
  ASSERT_TRUE(r.tail_truncated);
}

// A log cut at any byte must read as a prefix of the records written, with
// no error. This is the property crash recovery depends on.
TEST_F(LogTest, EveryCutPointYieldsACleanPrefix) {
  std::vector<size_t> boundaries = {0};
  auto add = [&](const std::string& record) {
    Write(record);
    boundaries.push_back(contents().size());
  };
  add("a");
  add("");
  add(BigString("first", 1000));
  add(BigString("spans-blocks", kBlockSize + 5000));
  add("after");
  add(BigString("fills-to-marginal-trailer", 2 * kBlockSize - contents().size() - 2 * kHeaderSize));
  add("x");
  add(BigString("three-blocks", 2 * kBlockSize + 100));
  add("tail");

  // Every offset near a record or block boundary, plus a sample of the rest.
  std::set<size_t> cuts;
  auto add_near = [&](size_t offset) {
    for (size_t d = 0; d <= 16; d++) {
      if (offset >= d) cuts.insert(offset - d);
      if (offset + d <= contents().size()) cuts.insert(offset + d);
    }
  };
  for (size_t b : boundaries) add_near(b);
  for (size_t b = 0; b <= contents().size(); b += kBlockSize) add_near(b);
  for (size_t offset = 0; offset < contents().size(); offset += 97) cuts.insert(offset);

  for (size_t cut : cuts) {
    const ReadResult r = ReadAll(Slice(contents().data(), cut));
    ASSERT_OK(r.status) << "cut=" << cut << " " << r.status.ToString();
    ASSERT_TRUE(IsPrefixOf(r.records, written_)) << "cut=" << cut;
    // Exactly the records that end at or before the cut are returned.
    const size_t complete =
        std::upper_bound(boundaries.begin(), boundaries.end(), cut) - boundaries.begin() - 1;
    ASSERT_EQ(complete, r.records.size()) << "cut=" << cut;
  }
}

TEST_F(LogTest, ZeroFilledTailIsCleanEnd) {
  Write("foo");
  Write("bar");
  // Power loss after the file was extended but before the data landed.
  contents().append(std::string(2 * kBlockSize + 100, '\0'));
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(written_, r.records);
  ASSERT_OK(r.status);
  ASSERT_TRUE(r.tail_truncated);
}

TEST_F(LogTest, ZeroedRegionFollowedByDataIsCorruption) {
  Write("foo");
  const size_t hole = contents().size();
  Write(BigString("bar", 100));
  Write("baz");
  std::fill(contents().begin() + hole, contents().begin() + hole + kHeaderSize + 100, '\0');
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_TRUE(r.status.IsCorruption());
}

// --- Damage: must be reported, never read past. ---

TEST_F(LogTest, ChecksumMismatchInPayload) {
  Write("foo");
  Write("bar");
  Write("baz");
  contents()[kHeaderSize + 3 + kHeaderSize + 1] ^= 0x01;  // inside "bar"
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, ChecksumMismatchInLastRecordIsNotATruncatedTail) {
  Write("foo");
  Write("bar");
  contents().back() ^= 0x40;
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, LengthBeyondBlockIsCorruption) {
  Write(BigString("foo", kBlockSize));  // makes block 0 a full block
  contents()[4] ^= 0x01;
  contents()[5] = static_cast<char>(0xff);
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, DamagedLengthAtTailIsNotMistakenForTruncation) {
  // Growing the length field of the final record makes it claim bytes the
  // file does not have, which is also what a torn write looks like. The
  // record is in fact complete, and the reader must notice.
  Write("foo");
  Write("bar");
  const size_t last_header = kHeaderSize + 3;
  contents()[last_header + 4] ^= 0x10;  // length 3 -> 19
  const ReadResult r = ReadAll(contents());
  ASSERT_EQ(std::vector<std::string>{"foo"}, r.records);
  ASSERT_TRUE(r.status.IsCorruption()) << r.status.ToString();
}

TEST_F(LogTest, UnexpectedMiddleType) {
  Write("foo");
  contents()[6] = kMiddleType;
  FixChecksum(0, 3);
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, UnexpectedLastType) {
  Write("foo");
  contents()[6] = kLastType;
  FixChecksum(0, 3);
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, UnexpectedFullTypeInsideFragmentedRecord) {
  Write("foo");
  Write("bar");
  contents()[6] = kFirstType;
  FixChecksum(0, 3);
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, UnexpectedFirstTypeInsideFragmentedRecord) {
  Write("foo");
  Write(BigString("bar", 100000));
  contents()[6] = kFirstType;
  FixChecksum(0, 3);
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, UnknownRecordType) {
  Write("foo");
  contents()[6] = static_cast<char>(kMaxRecordType + 1);
  FixChecksum(0, 3);
  const ReadResult r = ReadAll(contents());
  ASSERT_TRUE(r.records.empty());
  ASSERT_TRUE(r.status.IsCorruption());
}

TEST_F(LogTest, ErrorIsSticky) {
  Write("foo");
  Write("bar");
  contents()[kHeaderSize] ^= 0x01;
  StringSource source(contents());
  Reader reader(&source);
  Slice record;
  std::string scratch;
  ASSERT_FALSE(reader.ReadRecord(&record, &scratch));
  ASSERT_TRUE(reader.status().IsCorruption());
  ASSERT_FALSE(reader.ReadRecord(&record, &scratch));  // does not resume at "bar"
  ASSERT_TRUE(reader.status().IsCorruption());
}

// Flip every single bit of a log. The reader must either report corruption
// or return exactly what was written (possible only where the flipped bit
// is block-trailer padding). It must never return something else as success.
TEST_F(LogTest, EverySingleBitFlipIsDetected) {
  Write("a");
  Write("");
  Write(BigString("medium", 300));
  // Ends 4 bytes short of the block boundary, so those bytes are padding.
  Write(BigString("to-trailer", kBlockSize - 4 - contents().size() - 2 * kHeaderSize));
  Write("");
  ASSERT_EQ(static_cast<size_t>(kBlockSize - 4), contents().size());
  Write(BigString("spans", 40));  // forces the padding, then lands in block 1
  Write("end");

  const std::string original = contents();
  ASSERT_EQ(written_, ReadAll(original).records);

  size_t detected = 0, harmless = 0;
  // The big record's payload is uniform filler; flipping a sample of its
  // bits is as informative as flipping all of them.
  std::mt19937 rnd(17);
  for (size_t byte = 0; byte < original.size(); byte++) {
    const bool in_big_payload = byte > 400 && byte < static_cast<size_t>(kBlockSize) - 64;
    if (in_big_payload && rnd() % 50 != 0) continue;
    for (int bit = 0; bit < 8; bit++) {
      std::string damaged = original;
      damaged[byte] ^= static_cast<char>(1 << bit);
      const ReadResult r = ReadAll(damaged);
      if (r.status.IsCorruption()) {
        ASSERT_TRUE(IsPrefixOf(r.records, written_)) << "byte=" << byte << " bit=" << bit;
        detected++;
      } else {
        ASSERT_OK(r.status);
        ASSERT_EQ(written_, r.records) << "byte=" << byte << " bit=" << bit;
        const bool in_padding = byte >= static_cast<size_t>(kBlockSize) - 4 &&
                                byte < static_cast<size_t>(kBlockSize);
        ASSERT_TRUE(in_padding) << "byte=" << byte << " bit=" << bit;
        harmless++;
      }
    }
  }
  ASSERT_EQ(4u * 8u, harmless);
  ASSERT_GT(detected, 1000u);
}

}  // namespace lsm::log
