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

  ASSERT_TRUE(ParseFileName("CURRENT", &number, &type));
  ASSERT_EQ(0u, number);
  ASSERT_EQ(FileType::kCurrentFile, type);

  ASSERT_TRUE(ParseFileName("MANIFEST-000007", &number, &type));
  ASSERT_EQ(7u, number);
  ASSERT_EQ(FileType::kDescriptorFile, type);

  ASSERT_TRUE(ParseFileName("18.dbtmp", &number, &type));
  ASSERT_EQ(18u, number);
  ASSERT_EQ(FileType::kTempFile, type);

  for (const char* bad : {"", "foo", "foo-dx-100.log", ".log", "manifest", "LOC", "LOCKx", "100",
                          "100.", "100.lop", "100.logx", "100.sst2", "CURREN", "CURRENTX", "MANIFEST",
                          "MANIFEST-", "MANIFEST-3x", "MANIFEST--3", "XMANIFEST-3", "18446744073709551616.log", "-1.log",
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

  fname = DescriptorFileName("bar", 100);
  ASSERT_EQ("bar/MANIFEST-000100", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(100u, number);
  ASSERT_EQ(FileType::kDescriptorFile, type);

  fname = TempFileName("tmp", 999);
  ASSERT_EQ("tmp/000999.dbtmp", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(999u, number);
  ASSERT_EQ(FileType::kTempFile, type);

  fname = CurrentFileName("foo");
  ASSERT_EQ("foo/CURRENT", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(FileType::kCurrentFile, type);

  fname = LockFileName("foo");
  ASSERT_EQ("foo/LOCK", fname);
  ASSERT_TRUE(ParseFileName(fname.substr(4), &number, &type));
  ASSERT_EQ(FileType::kLockFile, type);
}

}  // namespace lsm
