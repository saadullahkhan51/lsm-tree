// The crash loop: open, run a random workload, lose power at a random
// point, reopen, and check what came back. Over and over, on the same
// database, so each cycle starts from whatever the last one left behind.
//
// How many cycles and with what seed is set by the environment:
//
//   LSM_CRASH_CYCLES   total crash-recovery cycles (default 300)
//   LSM_CRASH_SEED     base seed (default 1)
//
// A run is one database taken through kCyclesPerRun cycles; run r uses seed
// LSM_CRASH_SEED + r. A failure prints its run seed and cycle. The workload
// is a function of the seed, but the exact operation a crash lands on also
// depends on how the background thread is scheduled, so rerunning a seed
// replays the same workload with crashes in nearly, not exactly, the same
// places.

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "db/db_impl.h"
#include "fault_injection_env.h"
#include "gtest/gtest.h"
#include "lsm/db.h"
#include "testutil.h"

namespace lsm {

namespace {

constexpr int kCyclesPerRun = 25;
constexpr int kKeySpace = 150;
// Written in every batch, holding the number of batches applied so far.
// Batches are atomic, so after recovery its value says exactly which prefix
// of the history survived.
const char kMarkerKey[] = "!batches-applied";

uint64_t EnvOr(const char* name, uint64_t fallback) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::strtoull(value, nullptr, 10) : fallback;
}

struct Op {
  bool is_put;
  std::string key;
  std::string value;
};
using Batch = std::vector<Op>;
using Model = std::map<std::string, std::string>;

std::string Key(int k) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%04d", k);
  return buf;
}

void ApplyTo(Model* model, const Batch& batch) {
  for (const Op& op : batch) {
    if (op.is_put) {
      (*model)[op.key] = op.value;
    } else {
      model->erase(op.key);
    }
  }
}

// One database being crashed repeatedly, with the history that says what it
// should contain.
class CrashRun {
 public:
  CrashRun(const std::string& dbname, uint32_t seed) : dbname_(dbname), rnd_(seed) {
    // Small enough that every cycle flushes and compacts, so that crashes
    // land in the middle of table writes, manifest edits, and file deletes.
    options_.env = &env_;
    options_.write_buffer_size = 8 << 10;
    options_.max_file_size = 8 << 10;
    options_.max_bytes_for_level_base = 32 << 10;
    options_.level_size_multiplier = 4;
    options_.block_size = 1024;
  }

  ~CrashRun() { Close(); }

  // Runs one cycle. Failures are reported through gtest.
  void Cycle() {
    // Sometimes the power fails during recovery itself.
    if (rnd_() % 5 == 0) env_.CrashAfter(rnd_() % 60);
    Status s = DB::Open(options_, dbname_, &db_);
    if (!s.ok()) {
      ASSERT_FALSE(env_.IsFilesystemActive()) << "open failed without a crash: " << s.ToString();
      PowerLossAndRecover(/*pending=*/nullptr);
      if (testing::Test::HasFailure()) return;
    } else {
      // An open that completes must show everything the last cycle left.
      Verify();
      if (testing::Test::HasFailure()) return;
    }

    // Crash somewhere in the next stretch of work, or just past its end.
    env_.CrashAfter(rnd_() % (2 * ops_per_cycle_ + 1));
    const uint64_t ops_before = env_.MutatingOps();

    Batch pending;       // a write that failed: it may or may not have landed
    bool have_pending = false;
    const int steps = 20 + rnd_() % 150;
    for (int step = 0; step < steps && env_.IsFilesystemActive(); step++) {
      const uint32_t kind = rnd_() % 100;
      if (kind < 80) {
        Batch batch = RandomBatch();
        const bool sync = rnd_() % 4 == 0;
        s = Write(batch, sync);
        if (s.ok()) {
          history_.push_back(batch);
          ApplyTo(&model_, batch);
          if (sync) durable_ = history_.size();
        } else {
          ASSERT_FALSE(env_.IsFilesystemActive()) << "write failed without a crash: " << s.ToString();
          pending = std::move(batch);
          have_pending = true;
          break;
        }
      } else if (kind < 88) {
        const std::string key = Key(rnd_() % kKeySpace);
        std::string value;
        s = db_->Get(ReadOptions(), key, &value);
        auto it = model_.find(key);
        if (it == model_.end()) {
          ASSERT_TRUE(s.IsNotFound()) << key << ": " << s.ToString();
        } else {
          ASSERT_OK(s);
          ASSERT_EQ(it->second, value) << key;
        }
      } else if (kind < 92) {
        ASSERT_EQ(Render(model_), Scan());
      } else if (kind < 96) {
        (void)impl()->TEST_FlushMemTable();  // fails if the crash lands in it
      } else {
        const std::string a = Key(rnd_() % kKeySpace), b = Key(rnd_() % kKeySpace);
        const Slice begin = std::min(a, b), end = std::max(a, b);
        (void)db_->CompactRange(&begin, &end);
      }
      if (testing::Test::HasFailure()) return;
    }
    // Track how much I/O a cycle does, to keep crash points spread over it.
    if (env_.IsFilesystemActive()) {
      ops_per_cycle_ = (ops_per_cycle_ + (env_.MutatingOps() - ops_before)) / 2 + 1;
    }

    PowerLossAndRecover(have_pending ? &pending : nullptr);
    if (testing::Test::HasFailure()) return;
    Close();
  }

  size_t batches() const { return history_.size(); }

 private:
  DBImpl* impl() { return static_cast<DBImpl*>(db_); }

  void Close() {
    delete db_;
    db_ = nullptr;
  }

  Batch RandomBatch() {
    Batch batch;
    for (int i = 0, n = 1 + rnd_() % 6; i < n; i++) {
      Op op;
      op.key = Key(rnd_() % kKeySpace);
      op.is_put = rnd_() % 4 != 0;
      if (op.is_put) {
        // Mostly small; some span several blocks; a few span log blocks.
        const uint32_t r = rnd_() % 100;
        const size_t size = r < 80 ? 10 + rnd_() % 100 : r < 98 ? 1000 + rnd_() % 3000 : 40000;
        op.value = std::to_string(history_.size()) + ":" +
                   std::string(size, static_cast<char>('a' + rnd_() % 26));
      }
      batch.push_back(std::move(op));
    }
    return batch;
  }

  Status Write(const Batch& batch, bool sync) {
    WriteBatch wb;
    for (const Op& op : batch) {
      if (op.is_put) {
        wb.Put(op.key, op.value);
      } else {
        wb.Delete(op.key);
      }
    }
    wb.Put(kMarkerKey, std::to_string(history_.size() + 1));
    WriteOptions write_options;
    write_options.sync = sync;
    return db_->Write(write_options, &wb);
  }

  static std::string Render(const Model& model) {
    std::string out;
    for (const auto& [k, v] : model) out += "(" + k + "->" + v + ")";
    return out;
  }

  // A forward scan of everything but the marker.
  std::string Scan() {
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    std::string out;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      if (iter->key() == kMarkerKey) continue;
      out += "(" + std::string(iter->key()) + "->" + std::string(iter->value()) + ")";
    }
    EXPECT_OK(iter->status());
    return out;
  }

  // The power goes out now; the database is reopened; and what it holds is
  // checked against the history. *pending, if given, is a write that was in
  // flight and returned an error: it is allowed to have survived.
  void PowerLossAndRecover(const Batch* pending) {
    env_.SetFilesystemActive(false);
    Close();
    ASSERT_OK(env_.SimulateCrash(&rnd_));
    env_.SetFilesystemActive(true);

    Status s = DB::Open(options_, dbname_, &db_);
    ASSERT_OK(s) << "database did not reopen after a crash";

    // Which prefix of the history is there?
    std::string marker;
    s = db_->Get(ReadOptions(), kMarkerKey, &marker);
    ASSERT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
    const size_t recovered = s.ok() ? std::stoull(marker) : 0;

    ASSERT_GE(recovered, durable_) << "lost a write that was acknowledged with sync=true";
    ASSERT_LE(recovered, history_.size() + (pending != nullptr ? 1 : 0))
        << "recovered a write that was never issued";

    if (recovered == history_.size() + 1) {
      history_.push_back(*pending);
      ApplyTo(&model_, *pending);
    } else if (recovered < history_.size()) {
      history_.resize(recovered);
      model_.clear();
      for (const Batch& batch : history_) ApplyTo(&model_, batch);
    }
    // Whatever was recovered is on disk now and can no longer be lost.
    durable_ = history_.size();

    Verify();
  }

  // The database must hold exactly the state after history_: every key,
  // through point reads and through a scan, and a sound level structure.
  void Verify() {
    ASSERT_EQ(Render(model_), Scan()) << "state is not the history's state after "
                                      << history_.size() << " batches";
    for (int k = 0; k < kKeySpace; k++) {
      const std::string key = Key(k);
      std::string value;
      Status s = db_->Get(ReadOptions(), key, &value);
      auto it = model_.find(key);
      if (it == model_.end()) {
        ASSERT_TRUE(s.IsNotFound()) << key << ": " << s.ToString();
      } else {
        ASSERT_OK(s) << key;
        ASSERT_EQ(it->second, value) << key;
      }
    }
    ASSERT_OK(impl()->TEST_CheckInvariants());
  }

  const std::string dbname_;
  std::mt19937 rnd_;
  test::FaultInjectionEnv env_{Env::Default()};
  Options options_;
  DB* db_ = nullptr;

  std::vector<Batch> history_;  // every batch the database should hold, in order
  Model model_;                 // the state after history_
  size_t durable_ = 0;          // this many leading batches must survive a crash
  uint64_t ops_per_cycle_ = 300;
};

}  // namespace

TEST(CrashLoop, EveryAcknowledgedSyncWriteSurvivesAndStateIsAPrefix) {
  const uint64_t total_cycles = EnvOr("LSM_CRASH_CYCLES", 300);
  const uint64_t base_seed = EnvOr("LSM_CRASH_SEED", 1);
  const std::string dbname = test::NewTestDir("crash_loop");

  uint64_t cycles = 0;
  uint64_t batches = 0;
  for (uint64_t run = 0; cycles < total_cycles; run++) {
    const uint32_t seed = static_cast<uint32_t>(base_seed + run);
    std::filesystem::remove_all(dbname);
    CrashRun crash_run(dbname, seed);
    for (int cycle = 0; cycle < kCyclesPerRun && cycles < total_cycles; cycle++, cycles++) {
      SCOPED_TRACE("run seed " + std::to_string(seed) + ", cycle " + std::to_string(cycle));
      crash_run.Cycle();
      if (HasFailure()) return;
    }
    batches += crash_run.batches();
  }
  std::printf("  survived %llu crash-recovery cycles (%llu batches retained), base seed %llu\n",
              static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(batches),
              static_cast<unsigned long long>(base_seed));
}

// I/O errors instead of power loss: any operation may fail. Failures may
// surface as errors from any call, but a call that reports success must
// have told the truth, and once the errors stop and the database is
// reopened every acknowledged write must be there. Nothing is lost here
// that was acknowledged, synced or not, because the process never dies with
// data only in its own buffers.
TEST(CrashLoop, InjectedIOErrorsNeverProduceWrongData) {
  const uint64_t rounds = std::max<uint64_t>(EnvOr("LSM_CRASH_CYCLES", 300) / 10, 10);
  const uint64_t base_seed = EnvOr("LSM_CRASH_SEED", 1);
  const std::string dbname = test::NewTestDir("io_error_loop");

  test::FaultInjectionEnv env(Env::Default());
  Options options;
  options.env = &env;
  options.write_buffer_size = 8 << 10;
  options.max_file_size = 8 << 10;
  options.max_bytes_for_level_base = 32 << 10;
  options.level_size_multiplier = 4;

  std::mt19937 rnd(static_cast<uint32_t>(base_seed));
  Model model;
  uint64_t applied = 0;  // batches the database must hold
  uint64_t read_errors = 0, write_errors = 0, open_errors = 0;

  for (uint64_t round = 0; round < rounds; round++) {
    SCOPED_TRACE("seed " + std::to_string(base_seed) + ", round " + std::to_string(round));
    // Low enough that a round usually gets through several flushes and
    // compactions before a write fails and ends it.
    env.SetErrorRate(0.0003 + 0.003 * (rnd() % 100) / 100.0, rnd());

    DB* raw = nullptr;
    Status s = DB::Open(options, dbname, &raw);
    std::unique_ptr<DB> db(raw);
    if (!s.ok()) {
      ASSERT_TRUE(s.IsIOError()) << s.ToString();
      open_errors++;
    }

    Batch pending;
    bool have_pending = false;
    for (int step = 0; db != nullptr && step < 1000; step++) {
      if (rnd() % 4 != 0) {
        // One put or delete, plus the marker, as one batch.
        Batch batch(1);
        batch[0].key = Key(rnd() % kKeySpace);
        batch[0].is_put = rnd() % 4 != 0;
        batch[0].value = std::to_string(applied) + ":" + std::string(rnd() % 2000, 'v');
        WriteBatch wb;
        if (batch[0].is_put) {
          wb.Put(batch[0].key, batch[0].value);
        } else {
          wb.Delete(batch[0].key);
        }
        wb.Put(kMarkerKey, std::to_string(applied + 1));
        WriteOptions write_options;
        write_options.sync = rnd() % 4 == 0;
        s = db->Write(write_options, &wb);
        if (s.ok()) {
          ApplyTo(&model, batch);
          applied++;
        } else {
          // The database refuses all writes from here on; whether this one
          // landed is found out after the reopen.
          ASSERT_TRUE(s.IsIOError()) << s.ToString();
          write_errors++;
          pending = std::move(batch);
          have_pending = true;
          break;
        }
      } else {
        const std::string key = Key(rnd() % kKeySpace);
        std::string value;
        s = db->Get(ReadOptions(), key, &value);
        auto it = model.find(key);
        if (s.ok()) {
          ASSERT_TRUE(it != model.end()) << key << " read back after being deleted";
          ASSERT_EQ(it->second, value) << key;
        } else if (s.IsNotFound()) {
          ASSERT_TRUE(it == model.end()) << key << " reported missing";
        } else {
          ASSERT_TRUE(s.IsIOError()) << s.ToString();
          read_errors++;
        }
      }
    }

    // Stop the errors, restart, and take stock.
    db.reset();
    env.SetErrorRate(0);
    s = DB::Open(options, dbname, &raw);
    ASSERT_OK(s) << "database did not reopen once the errors stopped";
    db.reset(raw);

    std::string marker;
    s = db->Get(ReadOptions(), kMarkerKey, &marker);
    ASSERT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
    const uint64_t recovered = s.ok() ? std::stoull(marker) : 0;
    if (have_pending && recovered == applied + 1) {
      ApplyTo(&model, pending);
      applied++;
    }
    ASSERT_EQ(applied, recovered) << "an acknowledged write is missing";

    std::unique_ptr<Iterator> iter(db->NewIterator(ReadOptions()));
    Model scanned;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      if (iter->key() != kMarkerKey) scanned.emplace(iter->key(), iter->value());
    }
    ASSERT_OK(iter->status());
    ASSERT_EQ(model, scanned);
    iter.reset();
    ASSERT_OK(static_cast<DBImpl*>(db.get())->TEST_CheckInvariants());
  }

  std::printf("  %llu rounds: %llu failed opens, %llu failed writes, %llu failed reads, "
              "%llu errors injected\n",
              static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(open_errors),
              static_cast<unsigned long long>(write_errors),
              static_cast<unsigned long long>(read_errors),
              static_cast<unsigned long long>(env.InjectedErrors()));
  // The test is only meaningful if errors actually reached every path.
  EXPECT_GT(write_errors, 0u);
  EXPECT_GT(read_errors + open_errors, 0u);
}

}  // namespace lsm
