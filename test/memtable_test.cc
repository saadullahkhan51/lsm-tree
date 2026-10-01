#include "db/memtable.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace lsm {

namespace {

// How long the reader/writer stress runs. CI raises it to 30 for the TSan job.
int StressSeconds() {
  const char* env = std::getenv("LSM_STRESS_SECONDS");
  return env != nullptr ? std::atoi(env) : 2;
}

// "found:<value>", "deleted", or "missing".
std::string Get(const MemTable* mem, Slice key, SequenceNumber snapshot) {
  std::string value;
  Status s;
  if (!mem->Get(LookupKey(key, snapshot), &value, &s)) return "missing";
  if (s.IsNotFound()) return "deleted";
  return "found:" + value;
}

class MemTableTest : public testing::Test {
 protected:
  MemTableTest() : mem_(new MemTable(InternalKeyComparator(BytewiseComparator()))) { mem_->Ref(); }
  ~MemTableTest() override { mem_->Unref(); }

  MemTable* mem_;
};

}  // namespace

TEST_F(MemTableTest, EmptyFindsNothing) {
  ASSERT_EQ("missing", Get(mem_, "k", kMaxSequenceNumber));
}

TEST_F(MemTableTest, AddAndGet) {
  mem_->Add(1, kTypeValue, "a", "va");
  mem_->Add(2, kTypeValue, "b", "vb");
  mem_->Add(3, kTypeValue, "", "empty-key");
  mem_->Add(4, kTypeValue, "empty-value", "");

  ASSERT_EQ("found:va", Get(mem_, "a", kMaxSequenceNumber));
  ASSERT_EQ("found:vb", Get(mem_, "b", kMaxSequenceNumber));
  ASSERT_EQ("found:empty-key", Get(mem_, "", kMaxSequenceNumber));
  ASSERT_EQ("found:", Get(mem_, "empty-value", kMaxSequenceNumber));
  ASSERT_EQ("missing", Get(mem_, "c", kMaxSequenceNumber));
  // A key that is a prefix or extension of a stored key is a different key.
  ASSERT_EQ("missing", Get(mem_, "aa", kMaxSequenceNumber));
  ASSERT_EQ("missing", Get(mem_, "empty", kMaxSequenceNumber));
}

TEST_F(MemTableTest, ReadsSeeTheNewestVersionAtTheirSequence) {
  mem_->Add(10, kTypeValue, "k", "v10");
  mem_->Add(20, kTypeDeletion, "k", "");
  mem_->Add(30, kTypeValue, "k", "v30");

  ASSERT_EQ("missing", Get(mem_, "k", 9));
  ASSERT_EQ("found:v10", Get(mem_, "k", 10));
  ASSERT_EQ("found:v10", Get(mem_, "k", 19));
  ASSERT_EQ("deleted", Get(mem_, "k", 20));
  ASSERT_EQ("deleted", Get(mem_, "k", 29));
  ASSERT_EQ("found:v30", Get(mem_, "k", 30));
  ASSERT_EQ("found:v30", Get(mem_, "k", kMaxSequenceNumber));
}

TEST_F(MemTableTest, LargeEntries) {
  const std::string big_key(1000, 'k');
  const std::string big_value(100000, 'v');
  mem_->Add(1, kTypeValue, big_key, big_value);
  ASSERT_EQ("found:" + big_value, Get(mem_, big_key, 1));
}

TEST_F(MemTableTest, MemoryUsageGrowsWithData) {
  const size_t before = mem_->ApproximateMemoryUsage();
  for (int i = 0; i < 1000; i++) {
    mem_->Add(i + 1, kTypeValue, "key" + std::to_string(i), std::string(100, 'x'));
  }
  ASSERT_GE(mem_->ApproximateMemoryUsage(), before + 1000 * 100);
}

// The memtable's concurrency contract: one writer, many readers, no locks on
// the read path. Run under TSan this must report nothing.
//
// The writer adds entry i with sequence i + 1 for key (i % kKeys), then
// publishes i + 1 as the visible sequence, which is what the DB does with
// last_sequence_. A reader takes the visible sequence as its snapshot and
// must find exactly the newest version at or below it, even though newer
// entries for the same key may be mid-insert.
TEST_F(MemTableTest, ReadersRunLockFreeAgainstOneWriter) {
  constexpr int kReaders = 8;
  constexpr uint64_t kKeys = 1000;
  constexpr uint64_t kMaxEntries = 5'000'000;  // bounds memory, not time

  auto key_for = [](uint64_t k) { return "key" + std::to_string(k); };

  std::atomic<uint64_t> visible{0};
  std::atomic<bool> done{false};
  std::atomic<uint64_t> reads{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back([&, r] {
      std::mt19937_64 rnd(r);
      uint64_t local_reads = 0;
      while (!done.load(std::memory_order_acquire)) {
        const uint64_t snapshot = visible.load(std::memory_order_acquire);
        const uint64_t k = rnd() % kKeys;
        const std::string actual = Get(mem_, key_for(k), snapshot);
        // Entries visible at the snapshot are 0..snapshot-1; the newest one
        // for key k is the largest i < snapshot with i % kKeys == k.
        std::string expected = "missing";
        if (snapshot > k) {
          const uint64_t newest = ((snapshot - 1 - k) / kKeys) * kKeys + k;
          expected = "found:" + std::to_string(newest);
        }
        if (actual != expected) {
          ADD_FAILURE() << "key " << k << " at snapshot " << snapshot << ": expected " << expected
                        << ", got " << actual;
          return;
        }
        local_reads++;
      }
      reads.fetch_add(local_reads);
    });
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(StressSeconds());
  uint64_t i = 0;
  while (std::chrono::steady_clock::now() < deadline && !HasFailure()) {
    if (i < kMaxEntries) {
      for (int batch = 0; batch < 100; batch++, i++) {
        mem_->Add(i + 1, kTypeValue, key_for(i % kKeys), std::to_string(i));
        visible.store(i + 1, std::memory_order_release);
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  done.store(true, std::memory_order_release);
  for (std::thread& t : readers) t.join();

  ASSERT_GT(i, 0u);
  ASSERT_GT(reads.load(), 0u);
}

}  // namespace lsm
