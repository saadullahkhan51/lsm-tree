#include "db/skiplist.h"

#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "util/arena.h"
#include "util/hash.h"

namespace lsm {

namespace {

using Key = uint64_t;

struct KeyComparator {
  int operator()(const Key& a, const Key& b) const { return a < b ? -1 : (a > b ? +1 : 0); }
};

using List = SkipList<Key, KeyComparator>;

}  // namespace

TEST(SkipList, Empty) {
  Arena arena;
  List list(KeyComparator{}, &arena);
  ASSERT_FALSE(list.Contains(10));

  List::Iterator iter(&list);
  ASSERT_FALSE(iter.Valid());
  iter.SeekToFirst();
  ASSERT_FALSE(iter.Valid());
  iter.Seek(100);
  ASSERT_FALSE(iter.Valid());
  iter.SeekToLast();
  ASSERT_FALSE(iter.Valid());
}

TEST(SkipList, InsertAndLookupMatchStdSet) {
  const int kCount = 2000;
  const int kRange = 5000;
  std::mt19937 rnd(1000);
  std::set<Key> keys;
  Arena arena;
  List list(KeyComparator{}, &arena);
  for (int i = 0; i < kCount; i++) {
    const Key key = rnd() % kRange;
    if (keys.insert(key).second) list.Insert(key);
  }

  for (Key i = 0; i < kRange; i++) {
    ASSERT_EQ(keys.count(i) > 0, list.Contains(i));
  }

  {
    List::Iterator iter(&list);
    iter.SeekToFirst();
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(*keys.begin(), iter.key());
    iter.SeekToLast();
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(*keys.rbegin(), iter.key());
  }

  // Seek to every position, then walk forward a few steps.
  for (Key i = 0; i < kRange; i++) {
    List::Iterator iter(&list);
    iter.Seek(i);
    auto model = keys.lower_bound(i);
    for (int step = 0; step < 3; step++) {
      if (model == keys.end()) {
        ASSERT_FALSE(iter.Valid());
        break;
      }
      ASSERT_TRUE(iter.Valid());
      ASSERT_EQ(*model, iter.key());
      ++model;
      iter.Next();
    }
  }

  // Walk the whole list backward.
  {
    List::Iterator iter(&list);
    iter.SeekToLast();
    for (auto model = keys.rbegin(); model != keys.rend(); ++model) {
      ASSERT_TRUE(iter.Valid());
      ASSERT_EQ(*model, iter.key());
      iter.Prev();
    }
    ASSERT_FALSE(iter.Valid());
  }
}

// Checks that readers running without any locking always see a consistent
// list while one writer inserts.
//
// Keys are (key, generation, hash) packed into 64 bits. The writer inserts
// successive generations for each of K keys and then publishes the new
// generation. A reader snapshots the published generations, then iterates;
// everything published at snapshot time must be seen, and every key it does
// see must carry a valid hash (so it is not reading a half-built node).
class ConcurrentTest {
 public:
  static constexpr uint32_t K = 4;

  ConcurrentTest() : list_(KeyComparator{}, &arena_) {}

  // REQUIRES: one thread at a time.
  void WriteStep(std::mt19937* rnd) {
    const uint32_t k = (*rnd)() % K;
    const uint64_t g = current_.Get(k) + 1;
    list_.Insert(MakeKey(k, g));
    current_.Set(k, g);
  }

  void ReadStep(std::mt19937* rnd) {
    State initial_state;
    for (uint32_t k = 0; k < K; k++) {
      initial_state.Set(k, current_.Get(k));
    }

    Key pos = RandomTarget(rnd);
    List::Iterator iter(&list_);
    iter.Seek(pos);
    while (true) {
      Key current;
      if (!iter.Valid()) {
        current = MakeKey(K, 0);
      } else {
        current = iter.key();
        ASSERT_TRUE(IsValidKey(current)) << current;
      }
      ASSERT_LE(pos, current) << "should not go backwards";

      // Nothing in [pos, current) was found, so none of it may have been
      // present when the snapshot was taken.
      while (pos < current) {
        ASSERT_LT(key(pos), K) << pos;
        // Generation 0 is never inserted.
        ASSERT_TRUE(gen(pos) == 0 || gen(pos) > initial_state.Get(key(pos)))
            << "key: " << key(pos) << "; gen: " << gen(pos)
            << "; initgen: " << initial_state.Get(key(pos));

        if (key(pos) < key(current)) {
          pos = MakeKey(key(pos) + 1, 0);
        } else {
          pos = MakeKey(key(pos), gen(pos) + 1);
        }
      }

      if (!iter.Valid()) break;

      if ((*rnd)() % 2) {
        iter.Next();
        pos = MakeKey(key(pos), gen(pos) + 1);
      } else {
        const Key new_target = RandomTarget(rnd);
        if (new_target > pos) {
          pos = new_target;
          iter.Seek(new_target);
        }
      }
    }
  }

 private:
  static uint64_t key(Key k) { return k >> 40; }
  static uint64_t gen(Key k) { return (k >> 8) & 0xffffffffu; }
  static uint64_t hash(Key k) { return k & 0xff; }

  static uint64_t HashNumbers(uint64_t k, uint64_t g) {
    const uint64_t data[2] = {k, g};
    return Hash(reinterpret_cast<const char*>(data), sizeof(data), 0);
  }

  static Key MakeKey(uint64_t k, uint64_t g) {
    assert(k <= K);  // K itself is used as an end marker
    assert(g <= 0xffffffffu);
    return (k << 40) | (g << 8) | (HashNumbers(k, g) & 0xff);
  }

  static bool IsValidKey(Key k) { return hash(k) == (HashNumbers(key(k), gen(k)) & 0xff); }

  static Key RandomTarget(std::mt19937* rnd) {
    switch ((*rnd)() % 10) {
      case 0:
        return MakeKey(0, 0);  // the beginning
      case 1:
        return MakeKey(K, 0);  // the end
      default:
        return MakeKey((*rnd)() % K, 0);  // somewhere in the middle
    }
  }

  // Per-key generation most recently published by the writer.
  struct State {
    std::atomic<uint64_t> generation[K];
    State() {
      for (uint32_t k = 0; k < K; k++) Set(k, 0);
    }
    void Set(uint32_t k, uint64_t v) { generation[k].store(v, std::memory_order_release); }
    uint64_t Get(uint32_t k) { return generation[k].load(std::memory_order_acquire); }
  };

  State current_;
  Arena arena_;
  List list_;
};

// The checker itself must hold with no concurrency at all.
TEST(SkipList, ConcurrentCheckerWithoutThreads) {
  ConcurrentTest test;
  std::mt19937 rnd(7);
  for (int i = 0; i < 10000; i++) {
    test.ReadStep(&rnd);
    test.WriteStep(&rnd);
  }
}

TEST(SkipList, ReadersSeeConsistentListDuringInserts) {
  const int kReaders = 4;
  const int kWrites = 50000;

  ConcurrentTest test;
  std::atomic<bool> done{false};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back([&test, &done, r] {
      std::mt19937 rnd(100 + r);
      while (!done.load(std::memory_order_acquire)) {
        test.ReadStep(&rnd);
        if (testing::Test::HasFatalFailure()) return;
      }
    });
  }

  std::mt19937 rnd(99);
  for (int i = 0; i < kWrites; i++) {
    test.WriteStep(&rnd);
  }
  done.store(true, std::memory_order_release);
  for (std::thread& t : readers) t.join();
}

}  // namespace lsm
