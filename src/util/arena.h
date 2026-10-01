#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace lsm {

// Bump allocator. Memory is released only when the arena is destroyed,
// which is what lets memtable readers follow pointers without locks.
// Allocation is single-threaded; MemoryUsage may be read from any thread.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes);
  // Aligned for any pointer or 8-byte integer.
  char* AllocateAligned(size_t bytes);

  // Bytes obtained from the system, including per-block bookkeeping.
  size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace lsm
