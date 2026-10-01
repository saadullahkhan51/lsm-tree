#include "db/builder.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "fault_injection_env.h"
#include "gtest/gtest.h"
#include "testutil.h"
#include "util/cache.h"

namespace lsm {

// Flushing a memtable to an L0 table, and reading through memtable-then-
// table the way the database's Get does.
class BuilderTest : public testing::Test {
 protected:
  BuilderTest() : dbname_(test::NewTestDir("builder_test")) {
    options_.comparator = &icmp_;
    options_.block_size = 512;
    options_.block_cache = &block_cache_;
    table_cache_ = std::make_unique<TableCache>(dbname_, options_, env_, 100);
  }

  static MemTable* NewMemTable() {
    MemTable* mem = new MemTable(InternalKeyComparator(BytewiseComparator()));
    mem->Ref();
    return mem;
  }

  // Writes mem to table file `number`.
  Status Flush(MemTable* mem, uint64_t number, FileMetaData* meta) {
    meta->number = number;
    std::unique_ptr<Iterator> iter(mem->NewIterator());
    return BuildTable(dbname_, env_, options_, table_cache_.get(), iter.get(), meta);
  }

  // The read path for one table: the first entry at or after the lookup
  // key decides, if it is for the same user key.
  std::string GetFromTable(const FileMetaData& meta, Slice user_key, SequenceNumber snapshot) {
    struct Saver {
      Slice user_key;
      std::string result = "missing";
    } saver{user_key};
    const LookupKey lookup(user_key, snapshot);
    Status s = table_cache_->Get(ReadOptions(), meta.number, meta.file_size, lookup.internal_key(),
                                 &saver, [](void* arg, Slice key, Slice value) {
                                   auto* sv = static_cast<Saver*>(arg);
                                   ParsedInternalKey parsed;
                                   if (!ParseInternalKey(key, &parsed)) {
                                     sv->result = "corrupt";
                                   } else if (parsed.user_key == sv->user_key) {
                                     sv->result = parsed.type == kTypeValue
                                                      ? "found:" + std::string(value)
                                                      : "deleted";
                                   }
                                 });
    return s.ok() ? saver.result : s.ToString();
  }

  static std::string GetFromMemTable(MemTable* mem, Slice user_key, SequenceNumber snapshot) {
    std::string value;
    Status s;
    if (!mem->Get(LookupKey(user_key, snapshot), &value, &s)) return "missing";
    return s.IsNotFound() ? "deleted" : "found:" + value;
  }

  // Memtable first; the table only if the memtable has nothing for the key.
  std::string Get(MemTable* mem, const FileMetaData& meta, Slice user_key,
                  SequenceNumber snapshot = kMaxSequenceNumber) {
    const std::string from_mem = GetFromMemTable(mem, user_key, snapshot);
    return from_mem != "missing" ? from_mem : GetFromTable(meta, user_key, snapshot);
  }

  Env* env_ = Env::Default();
  std::string dbname_;
  InternalKeyComparator icmp_{BytewiseComparator()};
  ShardedLRUCache block_cache_{1 << 20};
  TableOptions options_;
  std::unique_ptr<TableCache> table_cache_;
};

TEST_F(BuilderTest, FlushedTableHoldsExactlyTheMemTable) {
  MemTable* mem = NewMemTable();
  SequenceNumber seq = 1;
  for (int i = 0; i < 1000; i++) {
    mem->Add(seq++, kTypeValue, "key" + std::to_string(i), "value" + std::to_string(i));
    if (i % 10 == 0) mem->Add(seq++, kTypeDeletion, "key" + std::to_string(i), "");
  }

  FileMetaData meta;
  ASSERT_OK(Flush(mem, 7, &meta));
  ASSERT_EQ(std::filesystem::file_size(TableFileName(dbname_, 7)), meta.file_size);

  std::unique_ptr<Iterator> mem_iter(mem->NewIterator());
  std::unique_ptr<Iterator> table_iter(
      table_cache_->NewIterator(ReadOptions(), meta.number, meta.file_size));
  mem_iter->SeekToFirst();
  table_iter->SeekToFirst();
  ASSERT_EQ(mem_iter->key(), meta.smallest.Encode());
  std::string last_key;
  for (; mem_iter->Valid(); mem_iter->Next(), table_iter->Next()) {
    ASSERT_TRUE(table_iter->Valid());
    ASSERT_EQ(mem_iter->key(), table_iter->key());
    ASSERT_EQ(mem_iter->value(), table_iter->value());
    last_key.assign(mem_iter->key());
  }
  ASSERT_FALSE(table_iter->Valid());
  ASSERT_OK(table_iter->status());
  ASSERT_EQ(last_key, meta.largest.Encode());
  mem->Unref();
}

TEST_F(BuilderTest, EmptyMemTableMakesNoFile) {
  MemTable* mem = NewMemTable();
  FileMetaData meta;
  ASSERT_OK(Flush(mem, 3, &meta));
  ASSERT_EQ(0u, meta.file_size);
  ASSERT_FALSE(env_->FileExists(TableFileName(dbname_, 3)));
  mem->Unref();
}

TEST_F(BuilderTest, GetReadsMemTableThenTable) {
  // An older generation, flushed to a table.
  MemTable* old_mem = NewMemTable();
  old_mem->Add(1, kTypeValue, "only-in-table", "t1");
  old_mem->Add(2, kTypeValue, "overwritten", "old");
  old_mem->Add(3, kTypeValue, "deleted-later", "doomed");
  old_mem->Add(4, kTypeValue, "deleted-in-table", "gone");
  old_mem->Add(5, kTypeDeletion, "deleted-in-table", "");
  FileMetaData meta;
  ASSERT_OK(Flush(old_mem, 1, &meta));
  old_mem->Unref();

  // A newer generation, still in memory.
  MemTable* mem = NewMemTable();
  mem->Add(6, kTypeValue, "overwritten", "new");
  mem->Add(7, kTypeDeletion, "deleted-later", "");
  mem->Add(8, kTypeValue, "only-in-mem", "m1");

  ASSERT_EQ("found:t1", Get(mem, meta, "only-in-table"));
  ASSERT_EQ("found:m1", Get(mem, meta, "only-in-mem"));
  ASSERT_EQ("found:new", Get(mem, meta, "overwritten"));
  // A tombstone in the memtable hides the table's value: the first match
  // wins and the table is not consulted.
  ASSERT_EQ("deleted", Get(mem, meta, "deleted-later"));
  ASSERT_EQ("deleted", Get(mem, meta, "deleted-in-table"));
  ASSERT_EQ("missing", Get(mem, meta, "never-written"));

  // Reads at an older sequence see through to the older versions.
  ASSERT_EQ("found:old", Get(mem, meta, "overwritten", 5));
  ASSERT_EQ("found:doomed", Get(mem, meta, "deleted-later", 6));
  ASSERT_EQ("found:gone", Get(mem, meta, "deleted-in-table", 4));
  ASSERT_EQ("missing", Get(mem, meta, "only-in-mem", 7));
  mem->Unref();
}

TEST_F(BuilderTest, FailedBuildLeavesNoFile) {
  test::FaultInjectionEnv fault_env(Env::Default());

  MemTable* mem = NewMemTable();
  for (int i = 0; i < 2000; i++) {
    mem->Add(i + 1, kTypeValue, "key" + std::to_string(i), "value");
  }

  FileMetaData meta;
  meta.number = 9;
  // The filesystem dies partway through the build.
  class DieAfterCreate : public EnvWrapper {
   public:
    explicit DieAfterCreate(test::FaultInjectionEnv* env) : EnvWrapper(env), env_(env) {}
    Status NewWritableFile(const std::string& f, std::unique_ptr<WritableFile>* r) override {
      Status s = target()->NewWritableFile(f, r);
      env_->SetFilesystemActive(false);
      return s;
    }
    Status RemoveFile(const std::string& f) override {
      env_->SetFilesystemActive(true);  // cleanup must be able to run
      return target()->RemoveFile(f);
    }
    test::FaultInjectionEnv* env_;
  } dying_env(&fault_env);

  std::unique_ptr<Iterator> iter(mem->NewIterator());
  Status s = BuildTable(dbname_, &dying_env, options_, table_cache_.get(), iter.get(), &meta);
  ASSERT_TRUE(s.IsIOError()) << s.ToString();
  ASSERT_EQ(0u, meta.file_size);
  ASSERT_FALSE(Env::Default()->FileExists(TableFileName(dbname_, 9)));
  mem->Unref();
}

TEST_F(BuilderTest, TableCacheReopensEvictedTables) {
  // More tables than the cache holds: reads must still work, by reopening.
  table_cache_ = std::make_unique<TableCache>(dbname_, options_, env_, 16);
  std::vector<FileMetaData> metas(64);
  for (size_t t = 0; t < metas.size(); t++) {
    MemTable* mem = NewMemTable();
    mem->Add(1, kTypeValue, "key", "from-table-" + std::to_string(t));
    ASSERT_OK(Flush(mem, 100 + t, &metas[t]));
    mem->Unref();
  }
  for (int round = 0; round < 2; round++) {
    for (size_t t = 0; t < metas.size(); t++) {
      ASSERT_EQ("found:from-table-" + std::to_string(t),
                GetFromTable(metas[t], "key", kMaxSequenceNumber));
    }
  }
}

TEST_F(BuilderTest, EvictDropsTheOpenFile) {
  MemTable* mem = NewMemTable();
  mem->Add(1, kTypeValue, "key", "value");
  FileMetaData meta;
  ASSERT_OK(Flush(mem, 5, &meta));
  mem->Unref();
  ASSERT_EQ("found:value", GetFromTable(meta, "key", kMaxSequenceNumber));

  // While the table is cached, the open descriptor keeps serving reads
  // even with the file unlinked. After Evict the file must be reopened,
  // which fails.
  ASSERT_OK(env_->RemoveFile(TableFileName(dbname_, 5)));
  table_cache_->Evict(5);
  const std::string result = GetFromTable(meta, "key", kMaxSequenceNumber);
  ASSERT_NE("found:value", result);
  ASSERT_NE("missing", result);  // an error, not a silent miss
}

TEST_F(BuilderTest, IteratorKeepsAnEvictedTableAlive) {
  MemTable* mem = NewMemTable();
  for (int i = 0; i < 500; i++) mem->Add(i + 1, kTypeValue, "key" + std::to_string(i), "v");
  FileMetaData meta;
  ASSERT_OK(Flush(mem, 6, &meta));
  mem->Unref();

  std::unique_ptr<Iterator> iter(
      table_cache_->NewIterator(ReadOptions(), meta.number, meta.file_size));
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());

  // Evicting and unlinking under a live iterator must not disturb it: the
  // iterator pins the table, and the table holds the descriptor.
  table_cache_->Evict(6);
  ASSERT_OK(env_->RemoveFile(TableFileName(dbname_, 6)));
  int n = 0;
  for (; iter->Valid(); iter->Next()) n++;
  ASSERT_OK(iter->status());
  ASSERT_EQ(500, n);
}

}  // namespace lsm
