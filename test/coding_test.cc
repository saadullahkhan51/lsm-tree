#include "util/coding.h"

#include <limits>
#include <vector>

#include "gtest/gtest.h"

namespace lsm {

TEST(Coding, Fixed32) {
  std::string s;
  for (uint32_t v = 0; v < 100000; v++) PutFixed32(&s, v);
  const char* p = s.data();
  for (uint32_t v = 0; v < 100000; v++, p += 4) ASSERT_EQ(v, DecodeFixed32(p));
}

TEST(Coding, Fixed64) {
  std::string s;
  for (int power = 0; power <= 63; power++) {
    uint64_t v = uint64_t{1} << power;
    PutFixed64(&s, v - 1);
    PutFixed64(&s, v);
    PutFixed64(&s, v + 1);
  }
  const char* p = s.data();
  for (int power = 0; power <= 63; power++, p += 24) {
    uint64_t v = uint64_t{1} << power;
    ASSERT_EQ(v - 1, DecodeFixed64(p));
    ASSERT_EQ(v, DecodeFixed64(p + 8));
    ASSERT_EQ(v + 1, DecodeFixed64(p + 16));
  }
}

TEST(Coding, FixedIsLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201);
  ASSERT_EQ(std::string("\x01\x02\x03\x04"), s);
  s.clear();
  PutFixed64(&s, 0x0807060504030201ull);
  ASSERT_EQ(std::string("\x01\x02\x03\x04\x05\x06\x07\x08"), s);
}

TEST(Coding, Varint32) {
  std::vector<uint32_t> values;
  for (uint32_t i = 0; i < 32 * 32; i++) values.push_back((i / 32) << (i % 32));
  values.push_back(std::numeric_limits<uint32_t>::max());

  std::string s;
  for (uint32_t v : values) PutVarint32(&s, v);

  const char* p = s.data();
  const char* limit = p + s.size();
  for (uint32_t expected : values) {
    uint32_t actual;
    const char* start = p;
    p = GetVarint32Ptr(p, limit, &actual);
    ASSERT_NE(nullptr, p);
    ASSERT_EQ(expected, actual);
    ASSERT_EQ(VarintLength(actual), p - start);
  }
  ASSERT_EQ(limit, p);
}

TEST(Coding, Varint64) {
  std::vector<uint64_t> values = {0, 100, ~uint64_t{0}, ~uint64_t{0} - 1};
  for (int k = 0; k < 64; k++) {
    const uint64_t power = uint64_t{1} << k;
    values.push_back(power - 1);
    values.push_back(power);
    values.push_back(power + 1);
  }

  std::string s;
  for (uint64_t v : values) PutVarint64(&s, v);

  Slice input(s);
  for (uint64_t expected : values) {
    uint64_t actual;
    const size_t before = input.size();
    ASSERT_TRUE(GetVarint64(&input, &actual));
    ASSERT_EQ(expected, actual);
    ASSERT_EQ(static_cast<size_t>(VarintLength(actual)), before - input.size());
  }
  ASSERT_TRUE(input.empty());
}

TEST(Coding, VarintLengthBoundaries) {
  ASSERT_EQ(1, VarintLength(0));
  ASSERT_EQ(1, VarintLength(127));
  ASSERT_EQ(2, VarintLength(128));
  ASSERT_EQ(5, VarintLength(std::numeric_limits<uint32_t>::max()));
  ASSERT_EQ(10, VarintLength(std::numeric_limits<uint64_t>::max()));
}

TEST(Coding, Varint32Overlong) {
  uint32_t result;
  std::string input("\x81\x82\x83\x84\x85\x11");  // continuation bit on the 5th byte
  ASSERT_EQ(nullptr, GetVarint32Ptr(input.data(), input.data() + input.size(), &result));
}

TEST(Coding, Varint64Overlong) {
  uint64_t result;
  std::string input("\x81\x82\x83\x84\x85\x81\x82\x83\x84\x85\x11");
  ASSERT_EQ(nullptr, GetVarint64Ptr(input.data(), input.data() + input.size(), &result));
}

TEST(Coding, VarintTruncation) {
  // Every strict prefix of an encoding must fail to parse.
  std::string s32, s64;
  PutVarint32(&s32, std::numeric_limits<uint32_t>::max());
  PutVarint64(&s64, std::numeric_limits<uint64_t>::max());
  uint32_t r32;
  uint64_t r64;
  for (size_t len = 0; len < s32.size(); len++) {
    ASSERT_EQ(nullptr, GetVarint32Ptr(s32.data(), s32.data() + len, &r32));
  }
  for (size_t len = 0; len < s64.size(); len++) {
    ASSERT_EQ(nullptr, GetVarint64Ptr(s64.data(), s64.data() + len, &r64));
  }
  ASSERT_NE(nullptr, GetVarint32Ptr(s32.data(), s32.data() + s32.size(), &r32));
  ASSERT_EQ(std::numeric_limits<uint32_t>::max(), r32);
  ASSERT_NE(nullptr, GetVarint64Ptr(s64.data(), s64.data() + s64.size(), &r64));
  ASSERT_EQ(std::numeric_limits<uint64_t>::max(), r64);
}

TEST(Coding, LengthPrefixedSlices) {
  std::string s;
  PutLengthPrefixedSlice(&s, "");
  PutLengthPrefixedSlice(&s, "foo");
  PutLengthPrefixedSlice(&s, "bar");
  PutLengthPrefixedSlice(&s, std::string(200, 'x'));

  Slice input(s);
  Slice v;
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  ASSERT_EQ("", v);
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  ASSERT_EQ("foo", v);
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  ASSERT_EQ("bar", v);
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  ASSERT_EQ(std::string(200, 'x'), v);
  ASSERT_TRUE(input.empty());
  ASSERT_FALSE(GetLengthPrefixedSlice(&input, &v));
}

TEST(Coding, LengthPrefixedSliceShortBody) {
  std::string s;
  PutVarint32(&s, 10);
  s.append("abc");  // claims 10 bytes, has 3
  Slice input(s);
  Slice v;
  ASSERT_FALSE(GetLengthPrefixedSlice(&input, &v));
}

}  // namespace lsm
