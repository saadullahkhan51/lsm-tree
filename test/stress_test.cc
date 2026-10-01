// The concurrency stress test: 16 threads doing mixed puts, deletes,
// batches, gets, snapshot reads, and scans against one database whose
// write buffer and level targets are small enough that flushes and
// compactions never stop. Run under TSan this is what checks the locking
// around the version list, the memtable handoff, and file deletion.
//
//   LSM_DB_STRESS_SECONDS   how long to run (default 3; the nightly job
//                           runs it for 600)
//
// Each thread owns the keys congruent to its index, and keeps a model of
// them. Nobody else writes those keys, so the thread knows exactly what
// every read of them must return, at any moment, without coordinating with
// the others. Keys it does not own it can still check for well-formedness.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "fault_injection_env.h"
#include "gtest/gtest.h"
#include "lsm/db.h"
#include "testutil.h"

namespace lsm {

namespace {

constexpr int kThreads = 16;
constexpr int kKeysPerThread = 150;

std::string Key(int index) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", index);
  return buf;
}

// Values name their key, so that a value filed under the wrong key is
// detectable even by a thread that does not own it.
std::string Value(const std::string& key, uint64_t version, size_t padding) {
  return key + ":" + std::to_string(version) + ":" + std::string(padding, 'x');
}

bool WellFormed(Slice key, Slice value) {
  return value.size() > key.size() && value.starts_with(key) && value[key.size()] == ':';
}

class Worker {
 public:
  Worker(DB* db, int index, std::atomic<bool>* stop, std::atomic<uint64_t>* failures)
      : db_(db), index_(index), stop_(stop), failures_(failures), rnd_(index) {}

  void Run() {
    while (!stop_->load(std::memory_order_relaxed) && failures_->load() == 0) {
      Step();
      ops_++;
    }
  }

  // After all threads have stopped: everything this thread wrote is there.
  void VerifyFinalState(DB* db) {
    for (int i = 0; i < kKeysPerThread; i++) CheckGet(db, ReadOptions(), OwnKey(i));
  }

  uint64_t ops() const { return ops_; }

 private:
  std::string OwnKey(int i) const { return Key(i * kThreads + index_); }
  std::string RandomOwnKey() { return OwnKey(rnd_() % kKeysPerThread); }

  void Fail(const std::string& what) {
    failures_->fetch_add(1);
    ADD_FAILURE() << "thread " << index_ << ": " << what;
  }

  void Step() {
    const uint32_t op = rnd_() % 100;
    if (op < 35) {
      const std::string key = RandomOwnKey();
      const std::string value = Value(key, ++version_, rnd_() % 5 == 0 ? rnd_() % 3000 : rnd_() % 100);
      if (Check(db_->Put(WriteOptions(), key, value))) model_[key] = value;
    } else if (op < 50) {
      const std::string key = RandomOwnKey();
      if (Check(db_->Delete(WriteOptions(), key))) model_.erase(key);
    } else if (op < 58) {
      // Several of this thread's keys in one atomic batch.
      WriteBatch batch;
      std::map<std::string, std::string> puts;
      std::vector<std::string> deletes;
      for (int i = 0, n = 2 + rnd_() % 6; i < n; i++) {
        const std::string key = RandomOwnKey();
        if (rnd_() % 4 == 0) {
          batch.Delete(key);
          puts.erase(key);
          deletes.push_back(key);
        } else {
          const std::string value = Value(key, ++version_, rnd_() % 200);
          batch.Put(key, value);
          puts[key] = value;
          std::erase(deletes, key);
        }
      }
      if (Check(db_->Write(WriteOptions(), &batch))) {
        for (const std::string& key : deletes) model_.erase(key);
        for (const auto& [key, value] : puts) model_[key] = value;
      }
    } else if (op < 80) {
      CheckGet(db_, ReadOptions(), RandomOwnKey());
    } else if (op < 85) {
      // Somebody else's key: any answer is possible, but not a malformed one.
      const std::string key = Key(rnd_() % (kThreads * kKeysPerThread));
      std::string value;
      Status s = db_->Get(ReadOptions(), key, &value);
      if (s.ok()) {
        if (!WellFormed(key, value)) Fail("key " + key + " holds another key's value");
      } else if (!s.IsNotFound()) {
        Fail("get: " + s.ToString());
      }
    } else if (op < 90) {
      // A snapshot, then reads through it. This thread writes nothing in
      // between, so its keys must read exactly as its model says.
      const Snapshot* snapshot = db_->GetSnapshot();
      ReadOptions read_options;
      read_options.snapshot = snapshot;
      for (int i = 0; i < 5; i++) CheckGet(db_, read_options, RandomOwnKey());
      db_->ReleaseSnapshot(snapshot);
    } else {
      Scan(rnd_() % 2 == 0);
    }
  }

  bool Check(const Status& s) {
    if (!s.ok()) Fail("write: " + s.ToString());
    return s.ok();
  }

  void CheckGet(DB* db, const ReadOptions& read_options, const std::string& key) {
    std::string value;
    Status s = db->Get(read_options, key, &value);
    auto it = model_.find(key);
    if (it == model_.end()) {
      if (!s.IsNotFound()) Fail("key " + key + " should be absent: " + s.ToString() + " " + value);
    } else if (!s.ok()) {
      Fail("key " + key + " should be present: " + s.ToString());
    } else if (value != it->second) {
      Fail("key " + key + " has the wrong value");
    }
  }

  // Scans a stretch of the key space in either direction. Every key this
  // thread owns in that stretch must appear exactly as in its model, in
  // order; everything else must be well-formed.
  void Scan(bool forward) {
    const int start = rnd_() % (kThreads * kKeysPerThread);
    const int length = 1 + rnd_() % 400;
    const std::string low = Key(start);
    const std::string high = Key(start + length);

    std::map<std::string, std::string> seen_own;
    std::string previous;
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    if (forward) {
      for (iter->Seek(low); iter->Valid() && iter->key() < Slice(high); iter->Next()) {
        if (!Visit(iter.get(), &previous, forward, &seen_own)) return;
      }
    } else {
      // Position on the last key below `high`.
      iter->Seek(high);
      if (iter->Valid()) {
        iter->Prev();
      } else {
        iter->SeekToLast();
      }
      for (; iter->Valid() && iter->key() >= Slice(low); iter->Prev()) {
        if (!Visit(iter.get(), &previous, forward, &seen_own)) return;
      }
    }
    if (!iter->status().ok()) {
      Fail("scan: " + iter->status().ToString());
      return;
    }

    std::map<std::string, std::string> expected(model_.lower_bound(low), model_.lower_bound(high));
    if (seen_own != expected) {
      Fail("scan of [" + low + ", " + high + ") saw " + std::to_string(seen_own.size()) +
           " of this thread's keys, expected " + std::to_string(expected.size()) +
           ", or saw different values");
    }
  }

  bool Visit(Iterator* iter, std::string* previous, bool forward,
             std::map<std::string, std::string>* seen_own) {
    const std::string key(iter->key());
    if (!previous->empty() && (forward ? key <= *previous : key >= *previous)) {
      Fail("scan went out of order at " + key);
      return false;
    }
    *previous = key;
    if (!WellFormed(key, iter->value())) {
      Fail("scan found key " + key + " holding another key's value");
      return false;
    }
    if (std::stoi(key.substr(3)) % kThreads == index_) {
      seen_own->emplace(key, iter->value());
    }
    return true;
  }

  DB* const db_;
  const int index_;
  std::atomic<bool>* const stop_;
  std::atomic<uint64_t>* const failures_;
  std::mt19937 rnd_;
  std::map<std::string, std::string> model_;  // this thread's keys only
  uint64_t version_ = 0;
  uint64_t ops_ = 0;
};

}  // namespace

TEST(Stress, SixteenThreadsAgainstPerThreadModels) {
  const char* env_seconds = std::getenv("LSM_DB_STRESS_SECONDS");
  const int seconds = env_seconds != nullptr ? std::atoi(env_seconds) : 3;
  const std::string dbname = test::NewTestDir("stress");

  test::FaultInjectionEnv env(Env::Default());  // skips real fsyncs
  Options options;
  options.env = &env;
  options.write_buffer_size = 32 << 10;
  options.max_file_size = 32 << 10;
  options.max_bytes_for_level_base = 128 << 10;
  options.level_size_multiplier = 4;

  DB* raw = nullptr;
  ASSERT_OK(DB::Open(options, dbname, &raw));
  std::unique_ptr<DB> db(raw);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> failures{0};
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    workers.push_back(std::make_unique<Worker>(db.get(), t, &stop, &failures));
  }
  for (auto& worker : workers) threads.emplace_back([&worker] { worker->Run(); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline && failures.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  stop.store(true);
  for (std::thread& t : threads) t.join();
  ASSERT_EQ(0u, failures.load());

  uint64_t total_ops = 0;
  for (auto& worker : workers) {
    worker->VerifyFinalState(db.get());
    total_ops += worker->ops();
  }
  auto* impl = static_cast<DBImpl*>(db.get());
  ASSERT_OK(impl->TEST_WaitForBackgroundWork());
  ASSERT_OK(impl->TEST_CheckInvariants());
  std::string levels, stalls;
  db->GetProperty("lsm.levels", &levels);
  db->GetProperty("lsm.stall-micros", &stalls);
  std::printf("  %llu operations in %d s across %d threads; writers stalled %s us\n%s",
              static_cast<unsigned long long>(total_ops), seconds, kThreads, stalls.c_str(),
              levels.c_str());

  // And it all survives a restart.
  db.reset();
  ASSERT_OK(DB::Open(options, dbname, &raw));
  db.reset(raw);
  for (auto& worker : workers) worker->VerifyFinalState(db.get());
  ASSERT_EQ(0u, failures.load());
}

}  // namespace lsm
