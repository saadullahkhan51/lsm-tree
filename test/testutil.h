#pragma once

#include <unistd.h>

#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "lsm/status.h"

#define ASSERT_OK(expr) ASSERT_TRUE((expr).ok())
#define EXPECT_OK(expr) EXPECT_TRUE((expr).ok())

namespace lsm::test {

// A fresh, empty directory unique to this process and name.
inline std::string NewTestDir(const std::string& name) {
  std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) /
                              ("lsm_" + name + "_" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir.string();
}

}  // namespace lsm::test
