#pragma once

#include <cstdint>
#include <string>

namespace lsm::test {

// The workload shared by lsm_kill_child and the test that checks up on it.
inline std::string KillChildKey(uint32_t i) { return "key" + std::to_string(i); }

// Sizes vary so that records land at every alignment in the log, and every
// 97th is large enough to span log blocks.
inline std::string KillChildValue(uint32_t i) {
  const size_t size = i % 97 == 0 ? 40000 + i % 1000 : 10 + i % 200;
  return std::string(size, static_cast<char>('a' + i % 26));
}

}  // namespace lsm::test
