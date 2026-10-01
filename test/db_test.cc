#include "lsm/db.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
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

  DBImpl* impl() { return static_cast<DBImpl*>(db_); }

  // Moves everything written so far out of the memtable into a table.
  void Flush() { ASSERT_OK(impl()->TEST_FlushMemTable()); }

  int NumFilesAtLevel(int level) { return impl()->TEST_NumFilesAtLevel(level); }

  int TotalTableFiles() {
    int total = 0;
    for (int level = 0; level < config::kNumLevels; level++) total += NumFilesAtLevel(level);
    return total;
  }

  uint64_t TotalTableBytes() {
    uint64_t total = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      total += impl()->TEST_NumBytesAtLevel(level);
    }
    return total;
  }

  // File counts per level with trailing zeros dropped: "2,0,1" means two
  // files in level 0 and one in level 2.
  std::string FilesPerLevel() {
    std::string result;
    size_t last_non_zero = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      const int n = NumFilesAtLevel(level);
      result += (level > 0 ? "," : "") + std::to_string(n);
      if (n > 0) last_non_zero = result.size();
    }
    result.resize(last_non_zero);
    return result;
  }

  // Lets flushes and automatic compactions run until none is needed.
  void WaitForBackground() { ASSERT_OK(impl()->TEST_WaitForBackgroundWork()); }

  // Compacts the whole of one level into the next.
  void CompactLevel(int level) { ASSERT_OK(impl()->TEST_CompactRange(level, nullptr, nullptr)); }

  std::string Property(const std::string& name) {
    std::string value;
    return db_->GetProperty(name, &value) ? value : "(unknown property)";
  }

  // Paths of the files of one type, lowest number first.
  std::vector<std::string> FilesOfType(FileType wanted) {
    std::vector<std::pair<uint64_t, std::string>> found;
    for (const auto& entry : std::filesystem::directory_iterator(dbname_)) {
      uint64_t number;
      FileType type;
      if (ParseFileName(entry.path().filename().string(), &number, &type) && type == wanted) {
        found.emplace_back(number, entry.path().string());
      }
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> paths;
    for (auto& [number, path] : found) paths.push_back(path);
    return paths;
  }

  std::vector<std::string> LogFiles() { return FilesOfType(FileType::kLogFile); }

  std::string Get(const std::string& key) {
    std::string value;
    Status s = db_->Get(ReadOptions(), key, &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    return s.ok() ? value : s.ToString();
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
  // An Env that skips real fsyncs, for tests that flush hundreds of times.
  // Declared before db_ so that it outlives the database.
  test::FaultInjectionEnv no_sync_env_{Env::Default()};
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

  // Recovery moved what it replayed into a table and retired the torn log.
  ASSERT_EQ(1, NumFilesAtLevel(0));
  ASSERT_EQ(1u, LogFiles().size());
  ASSERT_NE(logs[0], LogFiles()[0]);

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
  // Replace the live log with one whose second record skips a sequence
  // number, as if a record in between had vanished.
  Close();
  const std::vector<std::string> logs = LogFiles();
  ASSERT_EQ(1u, logs.size());
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(Env::Default()->NewWritableFile(logs[0], &file));
    log::Writer writer(file.get());
    WriteBatch batch;
    batch.Put("a", "1");
    WriteBatchInternal::SetSequence(&batch, 1);
    ASSERT_OK(writer.AddRecord(WriteBatchInternal::Contents(&batch)));
    WriteBatchInternal::SetSequence(&batch, 3);  // 2 is missing
    ASSERT_OK(writer.AddRecord(WriteBatchInternal::Contents(&batch)));
    ASSERT_OK(file->Close());
  }
  Status s = TryReopen();
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  ASSERT_NE(std::string::npos, s.ToString().find("sequence gap")) << s.ToString();
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

// --- Tables and versions ---

TEST_F(DBTest, GetFromTablesAfterFlush) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_OK(Put("bar", "b1"));
  Flush();
  ASSERT_EQ(1, NumFilesAtLevel(0));
  ASSERT_EQ("v1", Get("foo"));
  ASSERT_EQ("b1", Get("bar"));
  ASSERT_EQ("NOT_FOUND", Get("baz"));
}

TEST_F(DBTest, NewerDataShadowsOlderAcrossMemTableAndTables) {
  // Three generations of the same keys: oldest table, newer table, memtable.
  ASSERT_OK(Put("a", "a1"));
  ASSERT_OK(Put("b", "b1"));
  ASSERT_OK(Put("c", "c1"));
  ASSERT_OK(Put("d", "d1"));
  Flush();

  ASSERT_OK(Put("a", "a2"));
  ASSERT_OK(Delete("b"));
  Flush();
  ASSERT_EQ(2, NumFilesAtLevel(0));

  ASSERT_OK(Put("a", "a3"));
  ASSERT_OK(Delete("c"));

  ASSERT_EQ("a3", Get("a"));         // memtable over both tables
  ASSERT_EQ("NOT_FOUND", Get("b"));  // tombstone in the newer table hides the older value
  ASSERT_EQ("NOT_FOUND", Get("c"));  // tombstone in the memtable hides a table value
  ASSERT_EQ("d1", Get("d"));         // only in the oldest table

  // The same answers once everything is in tables, and after a restart.
  Flush();
  ASSERT_EQ(3, NumFilesAtLevel(0));
  for (int pass = 0; pass < 2; pass++) {
    ASSERT_EQ("a3", Get("a"));
    ASSERT_EQ("NOT_FOUND", Get("b"));
    ASSERT_EQ("NOT_FOUND", Get("c"));
    ASSERT_EQ("d1", Get("d"));
    Reopen();
  }
}

TEST_F(DBTest, DeletedKeyCanBeRewritten) {
  ASSERT_OK(Put("k", "v1"));
  Flush();
  ASSERT_OK(Delete("k"));
  Flush();
  ASSERT_OK(Put("k", "v2"));
  ASSERT_EQ("v2", Get("k"));
  Flush();
  ASSERT_EQ("v2", Get("k"));
}

TEST_F(DBTest, FullMemTableIsFlushedInTheBackground) {
  Close();
  options_.write_buffer_size = 64 << 10;
  Reopen();

  const std::string value(1000, 'x');
  for (int i = 0; i < 1000; i++) {  // about 1 MiB through a 64 KiB buffer
    ASSERT_OK(Put("key" + std::to_string(i), value + std::to_string(i)));
  }
  // Flushed many times over; compaction may have merged the tables since.
  ASSERT_GE(TotalTableFiles(), 1);
  ASSERT_LT(impl()->TEST_NumBytesAtLevel(0) + 0, uint64_t{2} << 20);
  for (int i = 0; i < 1000; i++) {
    ASSERT_EQ(value + std::to_string(i), Get("key" + std::to_string(i)));
  }
  ASSERT_OK(impl()->TEST_CheckInvariants());

  Reopen();
  for (int i = 0; i < 1000; i++) {
    ASSERT_EQ(value + std::to_string(i), Get("key" + std::to_string(i)));
  }
}

TEST_F(DBTest, ReadsStayCorrectWhileFlushesAndCompactionsRun) {
  Close();
  options_.write_buffer_size = 32 << 10;
  Reopen();

  // The writer overwrites a fixed set of keys with increasing versions and
  // publishes the last version it finished. Readers must never see a key
  // go backwards or disappear, whichever of the memtable, the immutable
  // memtable, or a table currently holds its newest version.
  constexpr int kKeys = 50;
  std::atomic<int> published{0};
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  auto key = [](int k) { return "key" + std::to_string(k); };
  auto value = [](int version) { return std::to_string(version) + std::string(500, 'v'); };

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; r++) {
    readers.emplace_back([&, r] {
      std::mt19937 rnd(r);
      while (!done.load()) {
        const int floor = published.load();
        std::string result;
        Status s = db_->Get(ReadOptions(), key(rnd() % kKeys), &result);
        if (floor == 0 && s.IsNotFound()) continue;
        if (!s.ok() || std::stoi(result) < floor) failures++;
      }
    });
  }

  for (int version = 1; version <= 60; version++) {
    for (int k = 0; k < kKeys; k++) ASSERT_OK(Put(key(k), value(version)));
    published.store(version);
  }
  done.store(true);
  for (std::thread& t : readers) t.join();
  ASSERT_EQ(0, failures.load());
  WaitForBackground();
  ASSERT_OK(impl()->TEST_CheckInvariants());
  ASSERT_LT(NumFilesAtLevel(0), config::kL0_CompactionTrigger);
  for (int k = 0; k < kKeys; k++) ASSERT_EQ(value(60), Get(key(k)));
}

TEST_F(DBTest, RecoveryFlushesTheLogAndRetiresIt) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_EQ(0, NumFilesAtLevel(0));
  const std::vector<std::string> before = LogFiles();
  ASSERT_EQ(1u, before.size());

  Reopen();
  ASSERT_EQ(1, NumFilesAtLevel(0));
  ASSERT_EQ("v1", Get("foo"));
  const std::vector<std::string> after = LogFiles();
  ASSERT_EQ(1u, after.size());
  ASSERT_NE(before[0], after[0]);

  // Reopening with nothing new written adds no table.
  Reopen();
  ASSERT_EQ(1, NumFilesAtLevel(0));
  ASSERT_EQ(1u, FilesOfType(FileType::kDescriptorFile).size());
  ASSERT_EQ(0u, FilesOfType(FileType::kTempFile).size());
}

TEST_F(DBTest, RecoveryWithALogLargerThanTheWriteBuffer) {
  // The log holds several memtables' worth; recovery must spill to tables
  // as it replays rather than hold it all in memory.
  const std::string value(10000, 'x');
  for (int i = 0; i < 100; i++) ASSERT_OK(Put("key" + std::to_string(i), value));
  Close();
  options_.write_buffer_size = 100 << 10;
  Reopen();
  // Recovery wrote about ten tables; nothing has compacted them yet,
  // because the background thread starts only after recovery.
  ASSERT_GE(TotalTableFiles(), 5);
  for (int i = 0; i < 100; i++) ASSERT_EQ(value, Get("key" + std::to_string(i)));
}

TEST_F(DBTest, ObsoleteFilesAreRemoved) {
  for (int round = 0; round < 3; round++) {
    ASSERT_OK(Put("key" + std::to_string(round), "v"));
    Flush();
  }
  // One log (the live one), one manifest, three tables.
  ASSERT_EQ(1u, LogFiles().size());
  ASSERT_EQ(1u, FilesOfType(FileType::kDescriptorFile).size());
  ASSERT_EQ(3u, FilesOfType(FileType::kTableFile).size());

  // A stray table no version refers to is cleaned up on open.
  Close();
  WriteFile(TableFileName(dbname_, 999999), "not a table, and nobody's table");
  Reopen();
  ASSERT_FALSE(std::filesystem::exists(TableFileName(dbname_, 999999)));
  ASSERT_EQ(3u, FilesOfType(FileType::kTableFile).size());
}

TEST_F(DBTest, MissingTableFailsOpen) {
  ASSERT_OK(Put("foo", "v1"));
  Flush();
  Close();
  const std::vector<std::string> tables = FilesOfType(FileType::kTableFile);
  ASSERT_EQ(1u, tables.size());
  std::filesystem::remove(tables[0]);

  Status s = TryReopen();
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  ASSERT_NE(std::string::npos, s.ToString().find("missing")) << s.ToString();
}

TEST_F(DBTest, DamagedTableBlockIsReportedNotSkipped) {
  for (int i = 0; i < 100; i++) {
    ASSERT_OK(Put("key" + std::to_string(i), std::string(100, static_cast<char>('a' + i % 26))));
  }
  Flush();
  Close();
  const std::vector<std::string> tables = FilesOfType(FileType::kTableFile);
  ASSERT_EQ(1u, tables.size());
  std::string contents = ReadFile(tables[0]);
  contents[contents.size() / 3] ^= 0x01;  // inside a data block
  WriteFile(tables[0], contents);

  ASSERT_OK(TryReopen());
  int corrupt = 0;
  for (int i = 0; i < 100; i++) {
    std::string value;
    Status s = db_->Get(ReadOptions(), "key" + std::to_string(i), &value);
    if (s.ok()) {
      ASSERT_EQ(std::string(100, static_cast<char>('a' + i % 26)), value) << i;
    } else {
      // Keys in the damaged block fail loudly. NotFound would be a lie.
      ASSERT_TRUE(s.IsCorruption()) << i << ": " << s.ToString();
      corrupt++;
    }
  }
  ASSERT_GT(corrupt, 0);
  ASSERT_LT(corrupt, 100);
}

TEST_F(DBTest, DamagedManifestFailsOpen) {
  ASSERT_OK(Put("foo", "v1"));
  Flush();
  Close();
  const std::vector<std::string> manifests = FilesOfType(FileType::kDescriptorFile);
  ASSERT_EQ(1u, manifests.size());
  std::string contents = ReadFile(manifests[0]);
  contents[contents.size() / 2] ^= 0x01;
  WriteFile(manifests[0], contents);

  Status s = TryReopen();
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(DBTest, MissingCurrentWithDataPresentIsNotTreatedAsANewDatabase) {
  ASSERT_OK(Put("foo", "v1"));
  Flush();
  Close();
  std::filesystem::remove(CurrentFileName(dbname_));

  // Starting a fresh manifest here would orphan the tables and the next
  // cleanup would delete them.
  Status s = TryReopen();
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  ASSERT_EQ(1u, FilesOfType(FileType::kTableFile).size());
}

TEST_F(DBTest, SequenceNumbersSurviveWhenEveryLogIsGone) {
  // After a flush and a reopen no log holds the old writes. New writes
  // must still get sequence numbers above them, or the old table entries
  // would shadow the new values.
  ASSERT_OK(Put("k", "old"));
  for (int i = 0; i < 100; i++) ASSERT_OK(Put("filler" + std::to_string(i), "x"));
  Flush();
  Reopen();
  Reopen();
  ASSERT_OK(Put("k", "new"));
  ASSERT_EQ("new", Get("k"));
  Flush();
  ASSERT_EQ("new", Get("k"));
  Reopen();
  ASSERT_EQ("new", Get("k"));
}

// --- Compaction ---

TEST_F(DBTest, FourLevelZeroFilesAreCompactedIntoLevelOne) {
  // Overlapping key ranges, so the files have to be merged, not moved.
  for (int round = 0; round < config::kL0_CompactionTrigger; round++) {
    for (int i = 0; i < 100; i++) {
      ASSERT_OK(Put("key" + std::to_string(i), "round" + std::to_string(round)));
    }
    Flush();
  }
  WaitForBackground();
  ASSERT_EQ(0, NumFilesAtLevel(0));
  ASSERT_EQ(1, NumFilesAtLevel(1));
  for (int i = 0; i < 100; i++) ASSERT_EQ("round3", Get("key" + std::to_string(i)));

  // The inputs are gone from disk.
  ASSERT_EQ(1u, FilesOfType(FileType::kTableFile).size());
  Reopen();
  ASSERT_EQ("0,1", FilesPerLevel());
  for (int i = 0; i < 100; i++) ASSERT_EQ("round3", Get("key" + std::to_string(i)));
}

TEST_F(DBTest, NonOverlappingFileMovesDownWithoutBeingRewritten) {
  for (int round = 0; round < config::kL0_CompactionTrigger; round++) {
    ASSERT_OK(Put("key" + std::to_string(round), "v"));  // disjoint ranges
    Flush();
  }
  const std::vector<std::string> before = FilesOfType(FileType::kTableFile);
  WaitForBackground();
  // One file was moved to level 1, which brings level 0 under its trigger.
  ASSERT_EQ("3,1", FilesPerLevel());
  ASSERT_EQ(before, FilesOfType(FileType::kTableFile));  // same files, none rewritten
  for (int round = 0; round < config::kL0_CompactionTrigger; round++) {
    ASSERT_EQ("v", Get("key" + std::to_string(round)));
  }
}

TEST_F(DBTest, CompactionDropsOverwrittenVersions) {
  const std::string value(1000, 'x');
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 100; i++) ASSERT_OK(Put("key" + std::to_string(i), value));
    Flush();
  }
  const uint64_t before = TotalTableBytes();
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  // Three copies of every key went in; one came out.
  ASSERT_LT(TotalTableBytes(), before / 2);
  ASSERT_GT(TotalTableBytes(), before / 4);
  for (int i = 0; i < 100; i++) ASSERT_EQ(value, Get("key" + std::to_string(i)));
}

TEST_F(DBTest, CompactingToTheBottomDropsDeletedKeysEntirely) {
  for (int i = 0; i < 100; i++) ASSERT_OK(Put("key" + std::to_string(i), "v"));
  Flush();
  for (int i = 0; i < 100; i++) ASSERT_OK(Delete("key" + std::to_string(i)));
  Flush();
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  // Values and tombstones cancelled out: nothing is left to store.
  ASSERT_EQ("", FilesPerLevel());
  ASSERT_EQ(0u, FilesOfType(FileType::kTableFile).size());
  for (int i = 0; i < 100; i++) ASSERT_EQ("NOT_FOUND", Get("key" + std::to_string(i)));
}

// The resurrection bug: a tombstone is compacted away while an older value
// for its key still sits in a deeper level, and the deleted key comes back.
TEST_F(DBTest, TombstoneIsKeptWhileADeeperLevelHoldsTheKey) {
  // Put the value two levels down.
  ASSERT_OK(Put("doomed", "v1"));
  ASSERT_OK(Put("bystander", "b1"));
  Flush();
  CompactLevel(0);
  CompactLevel(1);
  ASSERT_EQ("0,0,1", FilesPerLevel());

  // Delete it; the tombstone is in level 0.
  ASSERT_OK(Delete("doomed"));
  Flush();
  ASSERT_EQ("1,0,1", FilesPerLevel());
  ASSERT_EQ("NOT_FOUND", Get("doomed"));

  // Compact the tombstone into level 1. Level 1 holds nothing for the key,
  // so a compaction that looked only at its own inputs would conclude the
  // tombstone hides nothing and drop it. Level 2 still has "v1".
  CompactLevel(0);
  ASSERT_EQ("0,1,1", FilesPerLevel());
  ASSERT_EQ("NOT_FOUND", Get("doomed"));
  ASSERT_EQ("b1", Get("bystander"));

  Reopen();
  ASSERT_EQ("NOT_FOUND", Get("doomed"));

  // Now the tombstone meets the value. With nothing below, both go.
  CompactLevel(1);
  ASSERT_EQ("0,0,1", FilesPerLevel());
  ASSERT_EQ("NOT_FOUND", Get("doomed"));
  ASSERT_EQ("b1", Get("bystander"));
  Reopen();
  ASSERT_EQ("NOT_FOUND", Get("doomed"));
  ASSERT_EQ("b1", Get("bystander"));
}

TEST_F(DBTest, DeleteThenRewriteAcrossLevelsReadsTheNewestAtEveryStage) {
  ASSERT_OK(Put("k", "v1"));
  Flush();
  CompactLevel(0);
  CompactLevel(1);  // v1 in level 2

  ASSERT_OK(Delete("k"));
  Flush();
  CompactLevel(0);  // tombstone in level 1
  ASSERT_EQ("NOT_FOUND", Get("k"));

  ASSERT_OK(Put("k", "v2"));
  Flush();  // v2 in level 0
  ASSERT_EQ("1,1,1", FilesPerLevel());
  ASSERT_EQ("v2", Get("k"));

  CompactLevel(0);  // v2 over the tombstone
  ASSERT_EQ("v2", Get("k"));
  CompactLevel(1);  // over v1
  ASSERT_EQ("0,0,1", FilesPerLevel());
  ASSERT_EQ("v2", Get("k"));
}

TEST_F(DBTest, CompactRangeWithBoundsLeavesTheRestAlone) {
  for (const char* k : {"a", "m", "z"}) {
    ASSERT_OK(Put(k, "v"));
    Flush();
  }
  ASSERT_EQ("3", FilesPerLevel());
  const Slice begin = "l", end = "n";
  ASSERT_OK(db_->CompactRange(&begin, &end));
  ASSERT_EQ("2,1", FilesPerLevel());  // only the file holding "m" moved
  for (const char* k : {"a", "m", "z"}) ASSERT_EQ("v", Get(k));
}

TEST_F(DBTest, Properties) {
  ASSERT_EQ("0", Property("lsm.num-files-at-level0"));
  ASSERT_OK(Put("k", "v"));
  Flush();
  ASSERT_EQ("1", Property("lsm.num-files-at-level0"));
  ASSERT_EQ("0", Property("lsm.num-files-at-level6"));
  ASSERT_NE(std::string::npos, Property("lsm.levels").find("level 0: 1 files"));
  ASSERT_EQ("0", Property("lsm.stall-count"));
  ASSERT_EQ("0", Property("lsm.stall-micros"));

  for (const char* bad : {"lsm.num-files-at-level7", "lsm.num-files-at-level", "nonsense",
                          "lsm.num-files-at-level1x", "lsm.num-files-at-level-1", ""}) {
    ASSERT_EQ("(unknown property)", Property(bad)) << bad;
  }
}

TEST_F(DBTest, WritersStallWhenTheBackgroundThreadFallsBehind) {
  Close();
  options_.write_buffer_size = 4 << 10;
  options_.env = &no_sync_env_;
  Reopen();
  // Every write fills the memtable, and it has to be written to a table
  // before the next one can be; writers cannot avoid waiting. (4 KiB is
  // also smaller than a memtable's first arena block, which once made an
  // empty memtable look full.)
  const std::string value(1000, 'x');
  for (int i = 0; i < 500; i++) ASSERT_OK(Put("key" + std::to_string(i % 50), value));
  ASSERT_GT(std::stoull(Property("lsm.stall-count")), 0u);
  ASSERT_GT(std::stoull(Property("lsm.stall-micros")), 0u);
  for (int i = 0; i < 50; i++) ASSERT_EQ(value, Get("key" + std::to_string(i)));
}

// Small buffers, small tables, and small level targets, so that a few
// megabytes of writes spread across several levels.
class DBCompactionTest : public DBTest {
 protected:
  DBCompactionTest() {
    Close();
    options_.write_buffer_size = 16 << 10;
    options_.max_file_size = 16 << 10;
    options_.max_bytes_for_level_base = 64 << 10;
    options_.level_size_multiplier = 4;
    options_.env = &no_sync_env_;
    Reopen();
  }

  void VerifyAgainst(const std::map<std::string, std::string>& model, int key_space) {
    for (int k = 0; k < key_space; k++) {
      const std::string key = Key(k);
      auto it = model.find(key);
      ASSERT_EQ(it == model.end() ? "NOT_FOUND" : it->second, Get(key)) << key;
    }
  }

  static std::string Key(int k) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%06d", k);
    return buf;
  }
};

TEST_F(DBCompactionTest, RandomWritesSpreadAcrossLevelsAndStayReadable) {
  constexpr int kKeySpace = 2000;
  std::mt19937 rnd(301);
  std::map<std::string, std::string> model;
  for (int i = 0; i < 12000; i++) {
    const std::string key = Key(rnd() % kKeySpace);
    if (rnd() % 5 == 0) {
      ASSERT_OK(Delete(key));
      model.erase(key);
    } else {
      const std::string value = std::to_string(i) + std::string(100 + rnd() % 300, 'v');
      ASSERT_OK(Put(key, value));
      model[key] = value;
    }
  }
  WaitForBackground();
  ASSERT_OK(impl()->TEST_CheckInvariants());
  ASSERT_GT(NumFilesAtLevel(2) + NumFilesAtLevel(3), 0) << Property("lsm.levels");
  VerifyAgainst(model, kKeySpace);

  // Every table on disk belongs to the current version: compaction inputs
  // have been deleted.
  ASSERT_EQ(static_cast<size_t>(TotalTableFiles()), FilesOfType(FileType::kTableFile).size());

  Reopen();
  ASSERT_OK(impl()->TEST_CheckInvariants());
  VerifyAgainst(model, kKeySpace);

  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  ASSERT_OK(impl()->TEST_CheckInvariants());
  VerifyAgainst(model, kKeySpace);
}

TEST_F(DBCompactionTest, ReadersSeeConsistentDataDuringMultiLevelCompaction) {
  constexpr int kKeys = 300;
  std::atomic<int> published{0};
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  auto value = [](int version) { return std::to_string(version) + std::string(400, 'v'); };

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; r++) {
    readers.emplace_back([&, r] {
      std::mt19937 rnd(r);
      while (!done.load()) {
        const int floor = published.load();
        std::string result;
        Status s = db_->Get(ReadOptions(), Key(rnd() % kKeys), &result);
        if (floor == 0 && s.IsNotFound()) continue;
        // A reader must never see an older version than one already
        // published, wherever compaction has moved the key meanwhile.
        if (!s.ok() || std::stoi(result) < floor) failures++;
      }
    });
  }
  for (int version = 1; version <= 40; version++) {
    for (int k = 0; k < kKeys; k++) ASSERT_OK(Put(Key(k), value(version)));
    published.store(version);
  }
  done.store(true);
  for (std::thread& t : readers) t.join();
  ASSERT_EQ(0, failures.load());

  WaitForBackground();
  ASSERT_OK(impl()->TEST_CheckInvariants());
  for (int k = 0; k < kKeys; k++) ASSERT_EQ(value(40), Get(Key(k)));
}

TEST_F(DBCompactionTest, OperationsAgainstAModelWithFlushesCompactionsAndRestarts) {
  constexpr int kKeySpace = 300;
  for (uint32_t seed = 0; seed < 5; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Close();
    std::filesystem::remove_all(dbname_);
    Reopen();
    std::mt19937 rnd(seed);
    std::map<std::string, std::string> model;

    for (int step = 0; step < 3000; step++) {
      const uint32_t op = rnd() % 1000;
      const std::string key = Key(rnd() % kKeySpace);
      if (op < 600) {
        const std::string value = std::to_string(step) + std::string(rnd() % 200, 'v');
        ASSERT_OK(Put(key, value));
        model[key] = value;
      } else if (op < 850) {
        ASSERT_OK(Delete(key));
        model.erase(key);
      } else if (op < 970) {
        ASSERT_EQ(model.count(key) ? model[key] : "NOT_FOUND", Get(key));
      } else if (op < 980) {
        Flush();
      } else if (op < 990) {
        const std::string a = Key(rnd() % kKeySpace), b = Key(rnd() % kKeySpace);
        const Slice begin = std::min(a, b), end = std::max(a, b);
        ASSERT_OK(db_->CompactRange(&begin, &end));
      } else if (op < 995) {
        ASSERT_OK(db_->CompactRange(nullptr, nullptr));
      } else {
        Reopen();
      }
      if (step % 500 == 499) {
        VerifyAgainst(model, kKeySpace);
        ASSERT_OK(impl()->TEST_CheckInvariants());
      }
      if (HasFatalFailure()) return;
    }
    VerifyAgainst(model, kKeySpace);
  }
}

}  // namespace lsm
