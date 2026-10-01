#include "util/crc32c.h"

#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace lsm::crc32c {

enum class Path { kDispatch, kPortable, kHardware };

// Runs the body against the dispatching entry point and each compiled path.
class Crc32cTest : public testing::TestWithParam<Path> {
 protected:
  void SetUp() override {
    if (GetParam() == Path::kHardware && !internal::HardwareAvailable()) {
      GTEST_SKIP() << "no hardware CRC32C";
    }
  }

  uint32_t Extend(uint32_t crc, const char* data, size_t n) {
    switch (GetParam()) {
      case Path::kPortable:
        return internal::ExtendPortable(crc, data, n);
      case Path::kHardware:
        return internal::ExtendHardware(crc, data, n);
      case Path::kDispatch:
        break;
    }
    return crc32c::Extend(crc, data, n);
  }
  uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
};

// Vectors from RFC 3720, section B.4.
TEST_P(Crc32cTest, StandardResults) {
  char buf[32];

  std::memset(buf, 0, sizeof(buf));
  ASSERT_EQ(0x8a9136aau, Value(buf, sizeof(buf)));

  std::memset(buf, 0xff, sizeof(buf));
  ASSERT_EQ(0x62a8ab43u, Value(buf, sizeof(buf)));

  for (int i = 0; i < 32; i++) buf[i] = static_cast<char>(i);
  ASSERT_EQ(0x46dd794eu, Value(buf, sizeof(buf)));

  for (int i = 0; i < 32; i++) buf[i] = static_cast<char>(31 - i);
  ASSERT_EQ(0x113fdb5cu, Value(buf, sizeof(buf)));

  const uint8_t iscsi_read[48] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  ASSERT_EQ(0xd9963a56u, Value(reinterpret_cast<const char*>(iscsi_read), sizeof(iscsi_read)));

  ASSERT_EQ(0xe3069283u, Value("123456789", 9));
  ASSERT_EQ(0u, Value("", 0));
}

TEST_P(Crc32cTest, ExtendMatchesOneShotAtEverySplit) {
  // Covers every tail length and alignment of the 8-byte loop.
  std::string data;
  for (int i = 0; i < 100; i++) data.push_back(static_cast<char>(i * 7 + 3));
  const uint32_t whole = Value(data.data(), data.size());
  for (size_t split = 0; split <= data.size(); split++) {
    uint32_t crc = Value(data.data(), split);
    crc = Extend(crc, data.data() + split, data.size() - split);
    ASSERT_EQ(whole, crc) << "split=" << split;
  }
}

TEST_P(Crc32cTest, DifferentInputsDiffer) { ASSERT_NE(Value("a", 1), Value("foo", 3)); }

std::string PathName(const testing::TestParamInfo<Path>& info) {
  switch (info.param) {
    case Path::kPortable:
      return "Portable";
    case Path::kHardware:
      return "Hardware";
    case Path::kDispatch:
      break;
  }
  return "Dispatch";
}

INSTANTIATE_TEST_SUITE_P(Paths, Crc32cTest,
                         testing::Values(Path::kDispatch, Path::kPortable, Path::kHardware),
                         PathName);

TEST(Crc32c, PathsAgreeOnLargeInput) {
  if (!internal::HardwareAvailable()) GTEST_SKIP() << "no hardware CRC32C";
  std::string data(100000, '\0');
  uint32_t x = 12345;
  for (char& c : data) {
    x = x * 1103515245 + 12345;
    c = static_cast<char>(x >> 16);
  }
  ASSERT_EQ(internal::ExtendPortable(0, data.data(), data.size()),
            internal::ExtendHardware(0, data.data(), data.size()));
}

TEST(Crc32c, Mask) {
  const uint32_t crc = Value("foo", 3);
  ASSERT_NE(crc, Mask(crc));
  ASSERT_NE(crc, Mask(Mask(crc)));
  ASSERT_EQ(crc, Unmask(Mask(crc)));
  ASSERT_EQ(crc, Unmask(Unmask(Mask(Mask(crc)))));
}

}  // namespace lsm::crc32c
