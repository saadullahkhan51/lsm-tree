#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lsm/slice.h"

namespace lsm {

// Thread-safe LRU cache, split into 16 independently locked shards by key
// hash. Backs both the block cache and the table cache.
//
// Entries are reference-counted. Insert and Lookup return a Handle that pins
// the entry; the caller must Release it. An entry is destroyed (its deleter
// called) once it is both evicted and unpinned.
class ShardedLRUCache {
 public:
  struct Handle;
  using Deleter = void (*)(Slice key, void* value);

  // capacity is in the same units as the charge passed to Insert.
  explicit ShardedLRUCache(size_t capacity);
  ~ShardedLRUCache();

  ShardedLRUCache(const ShardedLRUCache&) = delete;
  ShardedLRUCache& operator=(const ShardedLRUCache&) = delete;

  // Replaces any existing entry for key.
  Handle* Insert(Slice key, void* value, size_t charge, Deleter deleter);
  // Returns nullptr on a miss.
  Handle* Lookup(Slice key);
  void Release(Handle* handle);
  void* Value(Handle* handle);
  void Erase(Slice key);

  size_t TotalCharge() const;
  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }

 private:
  class Shard;
  static constexpr int kNumShards = 16;

  Shard& ShardFor(Slice key);

  std::unique_ptr<Shard[]> shards_;
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
};

}  // namespace lsm
