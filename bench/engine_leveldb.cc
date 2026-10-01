#include "engine.h"

#ifdef LSM_BENCH_HAVE_LEVELDB

#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"

namespace bench {

namespace {

leveldb::Slice ToSlice(std::string_view s) { return leveldb::Slice(s.data(), s.size()); }
std::string_view ToView(const leveldb::Slice& s) { return std::string_view(s.data(), s.size()); }

class LevelDBCursor : public Cursor {
 public:
  explicit LevelDBCursor(leveldb::Iterator* iter) : iter_(iter) {}
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void SeekToLast() override { iter_->SeekToLast(); }
  void Seek(std::string_view target) override { iter_->Seek(ToSlice(target)); }
  void Next() override { iter_->Next(); }
  void Prev() override { iter_->Prev(); }
  bool Valid() const override { return iter_->Valid(); }
  std::string_view key() const override { return ToView(iter_->key()); }
  std::string_view value() const override { return ToView(iter_->value()); }

 private:
  std::unique_ptr<leveldb::Iterator> iter_;
};

class LevelDBEngine : public Engine {
 public:
  ~LevelDBEngine() override { Close(); }

  const char* name() const override { return "leveldb"; }

  std::string Open(const EngineConfig& config) override {
    cache_.reset(leveldb::NewLRUCache(config.block_cache_size));
    if (config.bloom_bits_per_key > 0) {
      filter_.reset(leveldb::NewBloomFilterPolicy(config.bloom_bits_per_key));
    }
    leveldb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = config.write_buffer_size;
    options.block_cache = cache_.get();
    options.block_size = config.block_size;
    options.filter_policy = filter_.get();
    options.max_file_size = config.max_file_size;
    options.compression = leveldb::kNoCompression;
    read_options_.verify_checksums = config.verify_checksums;
    leveldb::DB* db = nullptr;
    leveldb::Status s = leveldb::DB::Open(options, config.path, &db);
    if (!s.ok()) return s.ToString();
    db_.reset(db);
    return "";
  }

  void Close() override { db_.reset(); }

  bool Put(std::string_view key, std::string_view value, bool sync) override {
    leveldb::WriteOptions options;
    options.sync = sync;
    return db_->Put(options, ToSlice(key), ToSlice(value)).ok();
  }

  bool Delete(std::string_view key, bool sync) override {
    leveldb::WriteOptions options;
    options.sync = sync;
    return db_->Delete(options, ToSlice(key)).ok();
  }

  bool Get(std::string_view key, std::string* value, bool* found) override {
    leveldb::Status s = db_->Get(read_options_, ToSlice(key), value);
    *found = s.ok();
    return s.ok() || s.IsNotFound();
  }

  std::unique_ptr<Cursor> NewCursor() override {
    return std::make_unique<LevelDBCursor>(db_->NewIterator(read_options_));
  }

  void CompactAll() override { db_->CompactRange(nullptr, nullptr); }

  std::string Stats() override {
    std::string stats;
    db_->GetProperty("leveldb.stats", &stats);
    return stats;
  }

 private:
  leveldb::ReadOptions read_options_;
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<const leveldb::FilterPolicy> filter_;
  std::unique_ptr<leveldb::DB> db_;
};

}  // namespace

std::unique_ptr<Engine> NewLevelDBEngine() { return std::make_unique<LevelDBEngine>(); }

}  // namespace bench

#else

namespace bench {
std::unique_ptr<Engine> NewLevelDBEngine() { return nullptr; }
}  // namespace bench

#endif
