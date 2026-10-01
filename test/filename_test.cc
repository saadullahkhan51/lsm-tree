#include "db/filename.h"

#include "gtest/gtest.h"

namespace lsm {

TEST(FileName, Parse) {
  uint64_t number;
  FileType type;

  ASSERT_TRUE(ParseFileName("100.log", &number, &type));
  ASSERT_EQ(100u, number);
  ASSERT_EQ(FileType::kLogFile, type);

  ASSERT_TRUE(ParseFileName("0.log", &number, &type));
  ASSERT_EQ(0u, number);

  ASSERT_TRUE(ParseFileName("18446744073709551615.log", &number, &type));
  ASSERT_EQ(18446744073709551615ull, number);

  ASSERT_TRUE(ParseFileName("7.sst", &number, &type));
  ASSERT_EQ(7u, number);
  ASSERT_EQ(FileType::kTableFile, type);

  ASSERT_TRUE(ParseFileName("LOCK", &number, &type));
  ASSERT_EQ(0u, number);
  ASSERT_EQ(FileType::kLockFile, type);

  for (const char* bad : {"", "foo", "foo-dx-100.log", ".log", "manifest", "LOC", "LOCKx", "100",
                          "100.", "100.lop", "100.logx", "100.sst2", "18446744073709551616.log", "-1.log",
                          " 100.log", "100 .log"}) {
    ASSERT_FALSE(ParseFileName(bad, &number, &type)) << bad;
  }
}

TEST(FileName, ConstructionRoundTrips) {
  uint64_t number;
  FileType type;

  std::string fname = LogFileName("foo", 192);
  ASSERT_EQ("foo/000192.log", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(192u, number);
  ASSERT_EQ(FileType::kLogFile, type);

  // Numbers wider than the zero padding still round-trip.
  fname = LogFileName("bar", 12345678901ull);
  ASSERT_EQ("bar/12345678901.log", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(12345678901ull, number);

  fname = TableFileName("bar", 200);
  ASSERT_EQ("bar/000200.sst", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(200u, number);
  ASSERT_EQ(FileType::kTableFile, type);

  fname = LockFileName("foo");
  ASSERT_EQ("foo/LOCK", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(FileType::kLockFile, type);
}

}  // namespace lsm
