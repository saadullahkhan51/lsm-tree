#include "table/format.h"

#include <string>

#include "gtest/gtest.h"
#include "lsm/options.h"
#include "testutil.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

namespace {

// A block followed by a valid trailer.
std::string BlockWithTrailer(const std::string& data, char type = kNoCompression) {
  std::string out = data;
  out.push_back(type);
  PutFixed32(&out, crc32c::Mask(crc32c::Value(out.data(), out.size())));
  return out;
}

Status Read(const std::string& file_contents, const BlockHandle& handle, std::string* block) {
  test::StringSource file(file_contents);
  BlockContents contents;
  Status s = ReadBlock(&file, file_contents.size(), handle, &contents);
  if (s.ok()) block->assign(contents.data.get(), contents.size);
  return s;
}

}  // namespace

TEST(BlockHandle, RoundTrip) {
  for (uint64_t offset : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128},
                          uint64_t{1} << 32, ~uint64_t{0}}) {
    for (uint64_t size : {uint64_t{0}, uint64_t{4096}, ~uint64_t{0}}) {
      std::string encoded;
      BlockHandle(offset, size).EncodeTo(&encoded);
      ASSERT_LE(encoded.size(), BlockHandle::kMaxEncodedLength);

      Slice input(encoded);
      BlockHandle decoded;
      ASSERT_OK(decoded.DecodeFrom(&input));
      ASSERT_TRUE(input.empty());
      ASSERT_EQ(offset, decoded.offset());
      ASSERT_EQ(size, decoded.size());
    }
  }
}

TEST(BlockHandle, TruncatedIsCorruption) {
  std::string encoded;
  BlockHandle(1 << 20, 1 << 20).EncodeTo(&encoded);
  for (size_t len = 0; len < encoded.size(); len++) {
    Slice input(encoded.data(), len);
    BlockHandle decoded;
    ASSERT_TRUE(decoded.DecodeFrom(&input).IsCorruption()) << len;
  }
}

TEST(Footer, RoundTrip) {
  Footer footer;
  footer.set_metaindex_handle(BlockHandle(1000, 50));
  footer.set_index_handle(BlockHandle(1055, 300));
  std::string encoded;
  footer.EncodeTo(&encoded);
  ASSERT_EQ(48u, encoded.size());
  ASSERT_EQ("LSMTABLE", encoded.substr(40));

  Footer decoded;
  ASSERT_OK(decoded.DecodeFrom(encoded));
  ASSERT_EQ(1000u, decoded.metaindex_handle().offset());
  ASSERT_EQ(50u, decoded.metaindex_handle().size());
  ASSERT_EQ(1055u, decoded.index_handle().offset());
  ASSERT_EQ(300u, decoded.index_handle().size());
}

TEST(Footer, RejectsBadMagicAndBadLength) {
  Footer footer;
  std::string encoded;
  footer.EncodeTo(&encoded);

  Footer decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded.substr(1)).IsCorruption());
  ASSERT_TRUE(decoded.DecodeFrom(encoded + "x").IsCorruption());

  for (size_t i = 40; i < 48; i++) {
    std::string bad = encoded;
    bad[i] ^= 0x01;
    ASSERT_TRUE(decoded.DecodeFrom(bad).IsCorruption()) << i;
  }
}

TEST(ReadBlock, ReturnsTheBlockWithoutItsTrailer) {
  const std::string file = "junk" + BlockWithTrailer("hello block") + "more junk";
  std::string block;
  ASSERT_OK(Read(file, BlockHandle(4, 11), &block));
  ASSERT_EQ("hello block", block);
}

TEST(ReadBlock, EmptyBlock) {
  const std::string file = BlockWithTrailer("");
  std::string block = "x";
  ASSERT_OK(Read(file, BlockHandle(0, 0), &block));
  ASSERT_EQ("", block);
}

TEST(ReadBlock, AnyBitFlipIsCorruption) {
  const std::string file = BlockWithTrailer("some block contents");
  const BlockHandle handle(0, file.size() - kBlockTrailerSize);
  std::string block;
  for (size_t byte = 0; byte < file.size(); byte++) {
    for (int bit = 0; bit < 8; bit++) {
      std::string damaged = file;
      damaged[byte] ^= static_cast<char>(1 << bit);
      ASSERT_TRUE(Read(damaged, handle, &block).IsCorruption()) << byte << ":" << bit;
    }
  }
}

TEST(ReadBlock, UnknownCompressionTypeIsCorruption) {
  // The checksum is valid, so only the type check can reject it.
  const std::string file = BlockWithTrailer("contents", /*type=*/1);
  std::string block;
  Status s = Read(file, BlockHandle(0, 8), &block);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(ReadBlock, HandleOutsideTheFileIsCorruption) {
  const std::string file = BlockWithTrailer("contents");
  std::string block;
  ASSERT_TRUE(Read(file, BlockHandle(0, 9), &block).IsCorruption());    // trailer runs off the end
  ASSERT_TRUE(Read(file, BlockHandle(100, 8), &block).IsCorruption());  // offset past the end
  // Sizes that would overflow offset + size, or be absurd to allocate.
  ASSERT_TRUE(Read(file, BlockHandle(1, ~uint64_t{0}), &block).IsCorruption());
  ASSERT_TRUE(Read(file, BlockHandle(~uint64_t{0}, 8), &block).IsCorruption());
  ASSERT_TRUE(Read(file, BlockHandle(0, uint64_t{1} << 60), &block).IsCorruption());
}

}  // namespace lsm
