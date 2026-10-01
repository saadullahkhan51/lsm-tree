#include "util/cache.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "util/coding.h"

namespace lsm {

namespace {

std::string EncodeKey(int k) {
  std::string result;
  PutFixed32(&result, k);
  return result;
}
int DecodeKey(Slice k) { return static_cast<int>(DecodeFixed32(k.data())); }
void* EncodeValue(uintptr_t v) { return reinterpret_cast<void*>(v); }
int DecodeValue(void* v) { return static_cast<int>(reinterpret_cast<uintptr_t>(v)); }

}  // namespace

class CacheTest : public testing::Test {
 protected:
  static constexpr int kCacheSize = 1000;

  CacheTest() { current_ = this; }

  static void Deleter(Slice key, void* v) {
    current_->deleted_keys_.push_back(DecodeKey(key));
    current_->deleted_values_.push_back(DecodeValue(v));
  }

  int Lookup(int key) {
    ShardedLRUCache::Handle* handle = cache_.Lookup(EncodeKey(key));
    const int r = handle == nullptr ? -1 : DecodeValue(cache_.Value(handle));
    if (handle != nullptr) cache_.Release(handle);
    return r;
  }

  void Insert(int key, int value, int charge = 1) {
    cache_.Release(InsertAndReturnHandle(key, value, charge));
  }

  ShardedLRUCache::Handle* InsertAndReturnHandle(int key, int value, int charge = 1) {
    return cache_.Insert(EncodeKey(key), EncodeValue(value), charge, &CacheTest::Deleter);
  }

  void Erase(int key) { cache_.Erase(EncodeKey(key)); }

  static CacheTest* current_;
  std::vector<int> deleted_keys_;
  std::vector<int> deleted_values_;
  ShardedLRUCache cache_{kCacheSize};
};
CacheTest* CacheTest::current_;

TEST_F(CacheTest, HitAndMiss) {
  ASSERT_EQ(-1, Lookup(100));

  Insert(100, 101);
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1, Lookup(200));

  Insert(200, 201);
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(201, Lookup(200));

  // Re-inserting a key replaces the entry and destroys the old one.
  Insert(100, 102);
  ASSERT_EQ(102, Lookup(100));
  ASSERT_EQ(1u, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);
}

TEST_F(CacheTest, Erase) {
  Erase(200);
  ASSERT_EQ(0u, deleted_keys_.size());

  Insert(100, 101);
  Insert(200, 201);
  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1u, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);

  Erase(100);
  ASSERT_EQ(1u, deleted_keys_.size());
}

TEST_F(CacheTest, EntriesArePinned) {
  Insert(100, 101);
  ShardedLRUCache::Handle* h1 = cache_.Lookup(EncodeKey(100));
  ASSERT_EQ(101, DecodeValue(cache_.Value(h1)));

  // Replaced, but h1 still pins the old value.
  Insert(100, 102);
  ShardedLRUCache::Handle* h2 = cache_.Lookup(EncodeKey(100));
  ASSERT_EQ(102, DecodeValue(cache_.Value(h2)));
  ASSERT_EQ(0u, deleted_keys_.size());

  cache_.Release(h1);
  ASSERT_EQ(1u, deleted_keys_.size());
  ASSERT_EQ(101, deleted_values_[0]);

  // Erased, but h2 still pins it.
  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(1u, deleted_keys_.size());

  cache_.Release(h2);
  ASSERT_EQ(2u, deleted_keys_.size());
  ASSERT_EQ(102, deleted_values_[1]);
}

TEST_F(CacheTest, EvictionPolicy) {
  Insert(100, 101);
  Insert(200, 201);
  Insert(300, 301);
  ShardedLRUCache::Handle* h = cache_.Lookup(EncodeKey(300));

  // Frequently used entries stay; pinned entries stay regardless.
  for (int i = 0; i < kCacheSize + 100; i++) {
    Insert(1000 + i, 2000 + i);
    ASSERT_EQ(2000 + i, Lookup(1000 + i));
    ASSERT_EQ(101, Lookup(100));
  }
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1, Lookup(200));
  ASSERT_EQ(301, Lookup(300));
  cache_.Release(h);
}

TEST_F(CacheTest, UseExceedsCacheSize) {
  // Pinned entries cannot be evicted, so usage may exceed capacity.
  std::vector<ShardedLRUCache::Handle*> handles;
  for (int i = 0; i < kCacheSize + 100; i++) {
    handles.push_back(InsertAndReturnHandle(1000 + i, 2000 + i));
  }
  for (int i = 0; i < kCacheSize + 100; i++) {
    ASSERT_EQ(2000 + i, Lookup(1000 + i));
  }
  ASSERT_GT(cache_.TotalCharge(), static_cast<size_t>(kCacheSize));
  for (ShardedLRUCache::Handle* h : handles) cache_.Release(h);
}

TEST_F(CacheTest, HeavyEntries) {
  // Charges, not entry counts, are what the capacity bounds.
  const int kLight = 1;
  const int kHeavy = 10;
  int added = 0;
  int index = 0;
  while (added < 2 * kCacheSize) {
    const int weight = (index & 1) ? kLight : kHeavy;
    Insert(index, 1000 + index, weight);
    added += weight;
    index++;
  }

  int cached_weight = 0;
  for (int i = 0; i < index; i++) {
    const int weight = (i & 1) ? kLight : kHeavy;
    const int r = Lookup(i);
    if (r >= 0) {
      cached_weight += weight;
      ASSERT_EQ(1000 + i, r);
    }
  }
  ASSERT_LE(cached_weight, kCacheSize + kCacheSize / 10);
  ASSERT_EQ(static_cast<size_t>(cached_weight), cache_.TotalCharge());
}

TEST_F(CacheTest, HitAndMissCounters) {
  Insert(1, 1);
  ASSERT_EQ(1, Lookup(1));
  ASSERT_EQ(1, Lookup(1));
  ASSERT_EQ(-1, Lookup(2));
  ASSERT_EQ(2u, cache_.hits());
  ASSERT_EQ(1u, cache_.misses());
}

TEST(Cache, ZeroCapacityDisablesCaching) {
  static int deleted;
  deleted = 0;
  ShardedLRUCache cache(0);
  ShardedLRUCache::Handle* h =
      cache.Insert("k", EncodeValue(5), 1, [](Slice, void*) { deleted++; });
  ASSERT_EQ(5, DecodeValue(cache.Value(h)));
  ASSERT_EQ(nullptr, cache.Lookup("k"));
  ASSERT_EQ(0, deleted);
  cache.Release(h);
  ASSERT_EQ(1, deleted);
}

TEST(Cache, DestructorFreesUnpinnedEntries) {
  static int deleted;
  deleted = 0;
  {
    ShardedLRUCache cache(100);
    for (int i = 0; i < 10; i++) {
      cache.Release(cache.Insert(EncodeKey(i), EncodeValue(i), 1, [](Slice, void*) { deleted++; }));
    }
    ASSERT_EQ(0, deleted);
  }
  ASSERT_EQ(10, deleted);
}

// Exercised under TSan: shards are hit from many threads at once.
TEST(Cache, ConcurrentAccess) {
  static std::atomic<int> deleted;
  deleted = 0;
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 20000;
  {
    ShardedLRUCache cache(256);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&cache, t] {
        uint32_t x = t + 1;
        for (int i = 0; i < kOpsPerThread; i++) {
          x = x * 1664525 + 1013904223;
          const int key = (x >> 16) % 1000;
          if (ShardedLRUCache::Handle* h = cache.Lookup(EncodeKey(key))) {
            EXPECT_EQ(key, DecodeValue(cache.Value(h)));
            cache.Release(h);
          } else {
            cache.Release(cache.Insert(EncodeKey(key), EncodeValue(key), 1,
                                       [](Slice, void*) { deleted++; }));
          }
          if (i % 64 == 0) cache.Erase(EncodeKey(key));
        }
      });
    }
    for (std::thread& t : threads) t.join();
    // Eviction runs on insert and skips pinned entries, so each shard can
    // overshoot by at most one entry per concurrent thread.
    ASSERT_LE(cache.TotalCharge(), 256u + 16u * kThreads);
  }
  ASSERT_GT(deleted.load(), 0);
}

}  // namespace lsm
