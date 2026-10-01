#include "lsm/db.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/write_batch_internal.h"
#include "fault_injection_env.h"
#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

class DBTest : public testing::Test {
 protected:
  DBTest() : dbname_(test::NewTestDir("db_test")) { Reopen(); }
  ~DBTest() override { delete db_; }

  void Close() {
    delete db_;
    db_ = nullptr;
  }

  Status TryReopen() {
    Close();
    return DB::Open(options_, dbname_, &db_);
  }

  void Reopen() { ASSERT_OK(TryReopen()); }

  Status Put(const std::string& key, const std::string& value, bool sync = false) {
    WriteOptions write_options;
    write_options.sync = sync;
    return db_->Put(write_options, key, value);
  }

  Status Delete(const std::string& key) { return db_->Delete(WriteOptions(), key); }

  std::string Get(const std::string& key) {
    std::string value;
    Status s = db_->Get(ReadOptions(), key, &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    return s.ok() ? value : s.ToString();
  }

  // Paths of the write-ahead logs, oldest first.
  std::vector<std::string> LogFiles() {
    std::vector<std::pair<uint64_t, std::string>> logs;
    for (const auto& entry : std::filesystem::directory_iterator(dbname_)) {
      uint64_t number;
      FileType type;
      if (ParseFileName(entry.path().filename().string(), &number, &type) &&
          type == FileType::kLogFile) {
        logs.emplace_back(number, entry.path().string());
      }
    }
    std::sort(logs.begin(), logs.end());
    std::vector<std::string> paths;
    for (auto& [number, path] : logs) paths.push_back(path);
    return paths;
  }

  static std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

  static void WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
  }

  std::string dbname_;
  Options options_;
  DB* db_ = nullptr;
};

TEST_F(DBTest, Empty) { ASSERT_EQ("NOT_FOUND", Get("foo")); }

TEST_F(DBTest, PutGet) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_EQ("v1", Get("foo"));
  ASSERT_OK(Put("bar", "v2"));
  ASSERT_OK(Put("foo", "v3"));
  ASSERT_EQ("v3", Get("foo"));
  ASSERT_EQ("v2", Get("bar"));
}

TEST_F(DBTest, EmptyKeyAndEmptyValue) {
  ASSERT_OK(Put("", "v1"));
  ASSERT_EQ("v1", Get(""));
  ASSERT_OK(Put("k", ""));
  ASSERT_EQ("", Get("k"));
}

TEST_F(DBTest, KeysAreBinarySafe) {
  const std::string k1("a\0b", 3);
  const std::string k2("a\0c", 3);
  ASSERT_OK(Put(k1, "one"));
  ASSERT_OK(Put(k2, "two"));
  ASSERT_OK(Put("a", "prefix"));
  ASSERT_EQ("one", Get(k1));
  ASSERT_EQ("two", Get(k2));
  ASSERT_EQ("prefix", Get("a"));
}

TEST_F(DBTest, Delete) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_OK(Delete("foo"));
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_OK(Delete("never-existed"));
  ASSERT_OK(Put("foo", "v2"));
  ASSERT_EQ("v2", Get("foo"));
}

TEST_F(DBTest, BatchAppliesInOrder) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  batch.Put("c", "3");
  batch.Put("c", "4");
  ASSERT_OK(db_->Write(WriteOptions(), &batch));
  ASSERT_EQ("NOT_FOUND", Get("a"));
  ASSERT_EQ("2", Get("b"));
  ASSERT_EQ("4", Get("c"));
}

TEST_F(DBTest, EmptyBatch) {
  WriteBatch batch;
  ASSERT_OK(db_->Write(WriteOptions(), &batch));
  ASSERT_OK(Put("foo", "v1"));
  Reopen();
  ASSERT_EQ("v1", Get("foo"));
}

TEST_F(DBTest, RecoversWritesAfterReopen) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_OK(Put("baz", "v5"));
  ASSERT_OK(Delete("baz"));

  Reopen();
  ASSERT_EQ("v1", Get("foo"));
  ASSERT_EQ("NOT_FOUND", Get("baz"));

  // Writes after a recovery must order after the recovered ones.
  ASSERT_OK(Put("foo", "v2"));
  ASSERT_OK(Put("baz", "v6"));
  Reopen();
  ASSERT_EQ("v2", Get("foo"));
  ASSERT_EQ("v6", Get("baz"));

  Reopen();  // a session with no writes in between
  ASSERT_OK(Delete("foo"));
  Reopen();
  ASSERT_EQ("NOT_FOUND", Get("foo"));
  ASSERT_EQ("v6", Get("baz"));
}

TEST_F(DBTest, RecoversValuesLargerThanALogBlock) {
  const std::string big1(100000, 'x');
  const std::string big2(1 << 20, 'y');
  ASSERT_OK(Put("big1", big1));
  ASSERT_OK(Put("big2", big2));
  ASSERT_OK(Put("small", "s"));
  Reopen();
  ASSERT_EQ(big1, Get("big1"));
  ASSERT_EQ(big2, Get("big2"));
  ASSERT_EQ("s", Get("small"));
}

TEST_F(DBTest, OpenCreatesTheDirectory) {
  const std::string fresh = dbname_ + "/nested";
  DB* db = nullptr;
  ASSERT_OK(DB::Open(options_, fresh, &db));
  ASSERT_OK(db->Put(WriteOptions(), "k", "v"));
  delete db;
  ASSERT_TRUE(std::filesystem::exists(fresh + "/LOCK"));
}

TEST_F(DBTest, OpenIsExclusive) {
  DB* second = nullptr;
  Status s = DB::Open(options_, dbname_, &second);
  ASSERT_FALSE(s.ok());
  ASSERT_EQ(nullptr, second);

  Close();
  ASSERT_OK(DB::Open(options_, dbname_, &second));
  delete second;
}

TEST_F(DBTest, TruncatedLogTailRecoversAPrefix) {
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(Put("key" + std::to_string(i), "value" + std::to_string(i)));
  }
  Close();

  const std::vector<std::string> logs = LogFiles();
  ASSERT_EQ(1u, logs.size());
  std::filesystem::resize_file(logs[0], std::filesystem::file_size(logs[0]) - 3);

  Reopen();
  for (int i = 0; i < 9; i++) {
    ASSERT_EQ("value" + std::to_string(i), Get("key" + std::to_string(i)));
  }
  ASSERT_EQ("NOT_FOUND", Get("key9"));

  // The torn log stays behind; new writes go to a new one and both replay.
  ASSERT_OK(Put("key9", "second-try"));
  Reopen();
  ASSERT_EQ("value8", Get("key8"));
  ASSERT_EQ("second-try", Get("key9"));
}

TEST_F(DBTest, CorruptLogFailsOpen) {
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(Put("key" + std::to_string(i), "value" + std::to_string(i)));
  }
  Close();

  const std::vector<std::string> logs = LogFiles();
  ASSERT_EQ(1u, logs.size());
  std::string contents = ReadFile(logs[0]);
  contents[contents.size() / 2] ^= 0x04;
  WriteFile(logs[0], contents);

  Status s = TryReopen();
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  ASSERT_EQ(nullptr, db_);
}

TEST_F(DBTest, MissingLogRecordFailsOpen) {
  // Build a log by hand whose second record skips a sequence number.
  Close();
  const std::string fresh = test::NewTestDir("db_test_gap");
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(Env::Default()->NewWritableFile(LogFileName(fresh, 1), &file));
    log::Writer writer(file.get());
    WriteBatch batch;
    batch.Put("a", "1");
    WriteBatchInternal::SetSequence(&batch, 1);
    ASSERT_OK(writer.AddRecord(WriteBatchInternal::Contents(&batch)));
    WriteBatchInternal::SetSequence(&batch, 3);  // 2 is missing
    ASSERT_OK(writer.AddRecord(WriteBatchInternal::Contents(&batch)));
    ASSERT_OK(file->Close());
  }
  DB* db = nullptr;
  Status s = DB::Open(options_, fresh, &db);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBTest, WriteErrorIsSticky) {
  Close();
  test::FaultInjectionEnv env(Env::Default());
  options_.env = &env;
  Reopen();

  ASSERT_OK(Put("before", "v", /*sync=*/true));

  env.SetFilesystemActive(false);
  ASSERT_TRUE(Put("during", "v").IsIOError());

  // The log may hold a partial record now, so the database refuses writes
  // even though the filesystem is back.
  env.SetFilesystemActive(true);
  ASSERT_TRUE(Put("after", "v").IsIOError());

  ASSERT_EQ("v", Get("before"));
  ASSERT_EQ("NOT_FOUND", Get("during"));
  ASSERT_EQ("NOT_FOUND", Get("after"));
  Close();
}

// Many writers at once: the leader of each group logs for all of them.
TEST_F(DBTest, GroupCommitWithManyWriters) {
  constexpr int kThreads = 64;
  constexpr int kWritesPerThread = 100;

  auto key = [](int t, int i) { return "t" + std::to_string(t) + "-" + std::to_string(i); };

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kWritesPerThread; i++) {
        // Batches of two, some synced, so groups mix sizes and sync modes.
        WriteBatch batch;
        batch.Put(key(t, i), "v" + std::to_string(i));
        batch.Put("last-" + std::to_string(t), std::to_string(i));
        WriteOptions write_options;
        write_options.sync = i % 25 == 0;
        if (!db_->Write(write_options, &batch).ok()) failures++;

        // A thread reads its own writes as soon as they are acknowledged.
        std::string value;
        if (!db_->Get(ReadOptions(), key(t, i), &value).ok() || value != "v" + std::to_string(i)) {
          failures++;
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();
  ASSERT_EQ(0, failures.load());

  auto verify = [&] {
    for (int t = 0; t < kThreads; t++) {
      for (int i = 0; i < kWritesPerThread; i++) {
        ASSERT_EQ("v" + std::to_string(i), Get(key(t, i)));
      }
      ASSERT_EQ(std::to_string(kWritesPerThread - 1), Get("last-" + std::to_string(t)));
    }
  };
  verify();
  Close();

  // Grouping happened: fewer log records than calls to Write. And the log
  // numbers every update consecutively across groups.
  const std::vector<std::string> logs = LogFiles();
  ASSERT_EQ(1u, logs.size());
  std::unique_ptr<SequentialFile> file;
  ASSERT_OK(Env::Default()->NewSequentialFile(logs[0], &file));
  log::Reader reader(file.get());
  Slice record;
  std::string scratch;
  WriteBatch batch;
  int records = 0;
  SequenceNumber next_sequence = 1;
  while (reader.ReadRecord(&record, &scratch)) {
    WriteBatchInternal::SetContents(&batch, record);
    ASSERT_EQ(next_sequence, WriteBatchInternal::Sequence(&batch));
    next_sequence += WriteBatchInternal::Count(&batch);
    records++;
  }
  ASSERT_OK(reader.status());
  ASSERT_EQ(static_cast<SequenceNumber>(kThreads * kWritesPerThread * 2 + 1), next_sequence);
  ASSERT_LT(records, kThreads * kWritesPerThread);

  Reopen();
  verify();
}

}  // namespace lsm
