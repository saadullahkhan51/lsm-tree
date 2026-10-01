#include <memory>
#include <random>
#include <string>

#include "fault_injection_env.h"
#include "gtest/gtest.h"
#include "lsm/db.h"
#include "testutil.h"

namespace lsm {

// Power-loss tests. The database runs on a FaultInjectionEnv; a "crash"
// stops the filesystem, tears down the DB object, and discards everything
// that was not synced.
class CrashTest : public testing::Test {
 protected:
  CrashTest() : dbname_(test::NewTestDir("crash_test")) { options_.env = &env_; }
  ~CrashTest() override { delete db_; }

  void Open() { ASSERT_OK(DB::Open(options_, dbname_, &db_)); }

  void Crash(std::mt19937* torn = nullptr) {
    env_.SetFilesystemActive(false);
    delete db_;
    db_ = nullptr;
    ASSERT_OK(env_.SimulateCrash(torn));
    env_.SetFilesystemActive(true);
  }

  Status Put(int i, bool sync) {
    WriteOptions write_options;
    write_options.sync = sync;
    return db_->Put(write_options, Key(i), Value(i));
  }

  bool Has(int i) {
    std::string value;
    Status s = db_->Get(ReadOptions(), Key(i), &value);
    EXPECT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
    if (s.ok()) EXPECT_EQ(Value(i), value) << "key " << i;
    return s.ok();
  }

  static std::string Key(int i) { return "key" + std::to_string(i); }

  // Mostly small, with an occasional value that spans several log blocks.
  static std::string Value(int i) {
    const size_t size = i % 41 == 0 ? 70000 + i : 20 + i % 300;
    return std::string(size, static_cast<char>('a' + i % 26));
  }

  test::FaultInjectionEnv env_{Env::Default()};
  std::string dbname_;
  Options options_;
  DB* db_ = nullptr;
};

TEST_F(CrashTest, SyncedWritesSurvive) {
  Open();
  for (int i = 0; i < 100; i++) ASSERT_OK(Put(i, /*sync=*/true));
  Crash();
  Open();
  for (int i = 0; i < 100; i++) ASSERT_TRUE(Has(i)) << i;
}

TEST_F(CrashTest, UnsyncedWritesAreLostButTheDatabaseOpens) {
  Open();
  for (int i = 0; i < 50; i++) ASSERT_OK(Put(i, /*sync=*/false));
  ASSERT_OK(Put(50, /*sync=*/true));  // syncing the log covers everything before it
  for (int i = 51; i < 100; i++) ASSERT_OK(Put(i, /*sync=*/false));
  Crash();
  Open();
  for (int i = 0; i <= 50; i++) ASSERT_TRUE(Has(i)) << i;
  for (int i = 51; i < 100; i++) ASSERT_FALSE(Has(i)) << i;
}

// The log file is created when the database opens. If the directory is not
// synced after that, the file itself can vanish on power loss, taking
// synced writes with it. The database does sync it (SyncedWritesSurvive
// passes); this shows what happens to an engine that does not.
TEST_F(CrashTest, WithoutTheDirectorySyncSyncedWritesAreLost) {
  env_.SetDropDirSyncs(true);
  Open();
  for (int i = 0; i < 10; i++) ASSERT_OK(Put(i, /*sync=*/true));
  Crash();
  env_.SetDropDirSyncs(false);
  Open();
  for (int i = 0; i < 10; i++) ASSERT_FALSE(Has(i)) << i;
}

// Crash with torn writes, over and over, in one database. After each crash
// what is recovered must be a prefix of everything acknowledged so far, at
// least as long as the last synced write; the database then carries on from
// there.
TEST_F(CrashTest, RepeatedTornCrashesRecoverAPrefixEveryTime) {
  for (uint32_t seed = 0; seed < 20; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rnd(seed);
    std::filesystem::remove_all(dbname_);

    int next = 0;       // next key to write; all keys below it were recovered or written
    int durable = 0;    // keys below this were synced, so they must survive
    for (int generation = 0; generation < 8; generation++) {
      SCOPED_TRACE("generation " + std::to_string(generation));
      Open();
      // Everything recovered by the previous generation is still there.
      for (int i = 0; i < next; i++) ASSERT_TRUE(Has(i)) << i;
      ASSERT_FALSE(Has(next));
      durable = next;  // recovery re-reads it from logs that are now fully on disk

      const int writes = rnd() % 120;
      for (int n = 0; n < writes; n++) {
        const bool sync = rnd() % 10 == 0;
        ASSERT_OK(Put(next, sync));
        next++;
        if (sync) durable = next;
      }

      Crash(&rnd);
      Open();
      int recovered = 0;
      while (Has(recovered)) recovered++;
      ASSERT_GE(recovered, durable) << "lost a synced write";
      ASSERT_LE(recovered, next);
      for (int i = recovered; i <= next; i++) ASSERT_FALSE(Has(i)) << "hole before key " << i;
      delete db_;
      db_ = nullptr;

      next = recovered;  // the lost tail is gone for good; reuse those keys
    }
  }
}

}  // namespace lsm
