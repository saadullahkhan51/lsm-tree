#include "util/arena.h"

#include <cassert>
#include <cstdint>

namespace lsm {

namespace {
constexpr size_t kBlockSize = 4096;
constexpr size_t kAlign = 8;
}  // namespace

char* Arena::AllocateAligned(size_t bytes) {
  size_t mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  size_t slop = mod == 0 ? 0 : kAlign - mod;
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // A fresh block from operator new[] is already aligned.
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
  return result;
}

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Give a large object its own block so the rest of the current block
    // is not wasted.
    return AllocateNewBlock(bytes);
  }
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  blocks_.push_back(std::make_unique<char[]>(block_bytes));
  memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
  return blocks_.back().get();
}

}  // namespace lsm
