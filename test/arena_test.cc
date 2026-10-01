#include "util/arena.h"

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace lsm {

TEST(Arena, Empty) {
  Arena arena;
  ASSERT_EQ(0u, arena.MemoryUsage());
}

TEST(Arena, AllocationsAreDisjointAndStable) {
  std::mt19937 rnd(301);
  std::vector<std::pair<size_t, char*>> allocated;
  Arena arena;
  size_t bytes = 0;
  for (int i = 0; i < 100000; i++) {
    size_t s;
    if (i % 4000 == 0) {
      s = rnd() % 6000;  // occasionally larger than a block
    } else {
      s = rnd() % 10 == 0 ? rnd() % 100 : rnd() % 20;
    }
    if (s == 0) s = 1;

    const bool aligned = rnd() % 10 == 0;
    char* r = aligned ? arena.AllocateAligned(s) : arena.Allocate(s);
    if (aligned) {
      ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(r) % 8);
    }

    // Fill with a pattern derived from the allocation's index; overlapping
    // allocations would corrupt each other's pattern.
    for (size_t b = 0; b < s; b++) r[b] = static_cast<char>(i % 256);
    bytes += s;
    allocated.emplace_back(s, r);
    ASSERT_GE(arena.MemoryUsage(), bytes);
    if (i > 100000 / 10) {
      ASSERT_LE(arena.MemoryUsage(), bytes * 1.10);
    }
  }
  for (size_t i = 0; i < allocated.size(); i++) {
    const auto [size, p] = allocated[i];
    for (size_t b = 0; b < size; b++) {
      ASSERT_EQ(static_cast<int>(i % 256), static_cast<int>(p[b]) & 0xff);
    }
  }
}

}  // namespace lsm
