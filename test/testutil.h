#pragma once

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "lsm/env.h"
#include "lsm/status.h"

#define ASSERT_OK(expr) ASSERT_TRUE(::lsm::test::IsOk(expr))
#define EXPECT_OK(expr) EXPECT_TRUE(::lsm::test::IsOk(expr))

namespace lsm::test {

// Fails with the status text rather than a bare "false".
inline ::testing::AssertionResult IsOk(const Status& s) {
  if (s.ok()) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure() << s.ToString();
}

// A WritableFile that appends to a string.
class StringSink : public WritableFile {
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

// A RandomAccessFile over a copy of a string.
class StringSource : public RandomAccessFile {
 public:
  explicit StringSource(Slice contents) : contents_(contents) {}

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    if (offset >= contents_.size()) {
      *result = Slice();
      return Status::OK();
    }
    n = std::min<uint64_t>(n, contents_.size() - offset);
    std::memcpy(scratch, contents_.data() + offset, n);
    *result = Slice(scratch, n);
    return Status::OK();
  }

 private:
  const std::string contents_;
};

// A fresh, empty directory unique to this process and name.
inline std::string NewTestDir(const std::string& name) {
  std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) /
                              ("lsm_" + name + "_" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir.string();
}

}  // namespace lsm::test
