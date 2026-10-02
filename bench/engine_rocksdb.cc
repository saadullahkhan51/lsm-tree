#include "engine.h"

#ifdef LSM_BENCH_HAVE_ROCKSDB

#include "rocksdb/cache.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/table.h"

namespace bench {

namespace {

rocksdb::Slice ToSlice(std::string_view s) { return rocksdb::Slice(s.data(), s.size()); }
std::string_view ToView(const rocksdb::Slice& s) { return std::string_view(s.data(), s.size()); }

// RocksDB 8 (Ubuntu 24.04's) opens into a raw pointer; RocksDB 11
// (Homebrew's) only into a unique_ptr. Use whichever this one has.
template <typename DB>
rocksdb::Status OpenDB(const rocksdb::Options& options, const std::string& path,
                       std::unique_ptr<DB>* db) {
  if constexpr (requires(DB** raw) { DB::Open(options, path, raw); }) {
    DB* raw = nullptr;
    rocksdb::Status s = DB::Open(options, path, &raw);
    db->reset(raw);
    return s;
  } else {
    return DB::Open(options, path, db);
  }
}

class RocksDBCursor : public Cursor {
 public:
  explicit RocksDBCursor(rocksdb::Iterator* iter) : iter_(iter) {}
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void SeekToLast() override { iter_->SeekToLast(); }
  void Seek(std::string_view target) override { iter_->Seek(ToSlice(target)); }
  void Next() override { iter_->Next(); }
  void Prev() override { iter_->Prev(); }
  bool Valid() const override { return iter_->Valid(); }
  std::string_view key() const override { return ToView(iter_->key()); }
  std::string_view value() const override { return ToView(iter_->value()); }

 private:
  std::unique_ptr<rocksdb::Iterator> iter_;
};

class RocksDBEngine : public Engine {
 public:
  ~RocksDBEngine() override { Close(); }

  const char* name() const override { return "rocksdb"; }

  std::string Open(const EngineConfig& config) override {
    // RocksDB as it ships, with the settings the comparison holds equal:
    // memtable size, block cache, block size, bloom bits, no compression.
    // Its level shape, background threads, and everything else are its
    // defaults.
    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = config.write_buffer_size;
    options.compression = rocksdb::kNoCompression;
    options.bottommost_compression = rocksdb::kNoCompression;

    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = rocksdb::NewLRUCache(config.block_cache_size);
    table_options.block_size = config.block_size;
    if (config.bloom_bits_per_key > 0) {
      table_options.filter_policy.reset(
          rocksdb::NewBloomFilterPolicy(config.bloom_bits_per_key, false));
    }
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    read_options_.verify_checksums = config.verify_checksums;
    rocksdb::Status s = OpenDB(options, config.path, &db_);
    return s.ok() ? "" : s.ToString();
  }

  void Close() override { db_.reset(); }

  bool Put(std::string_view key, std::string_view value, bool sync) override {
    rocksdb::WriteOptions options;
    options.sync = sync;
    return db_->Put(options, ToSlice(key), ToSlice(value)).ok();
  }

  bool Delete(std::string_view key, bool sync) override {
    rocksdb::WriteOptions options;
    options.sync = sync;
    return db_->Delete(options, ToSlice(key)).ok();
  }

  bool Get(std::string_view key, std::string* value, bool* found) override {
    rocksdb::Status s = db_->Get(read_options_, ToSlice(key), value);
    *found = s.ok();
    return s.ok() || s.IsNotFound();
  }

  std::unique_ptr<Cursor> NewCursor() override {
    return std::make_unique<RocksDBCursor>(db_->NewIterator(read_options_));
  }

  void CompactAll() override {
    (void)db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
  }

  void WaitForBackground() override {
    (void)db_->WaitForCompact(rocksdb::WaitForCompactOptions());
  }

  std::string Stats() override {
    std::string stats;
    db_->GetProperty("rocksdb.stats", &stats);
    return stats;
  }

 private:
  rocksdb::ReadOptions read_options_;
  std::unique_ptr<rocksdb::DB> db_;
};

}  // namespace

std::unique_ptr<Engine> NewRocksDBEngine() { return std::make_unique<RocksDBEngine>(); }

}  // namespace bench

#else

namespace bench {
std::unique_ptr<Engine> NewRocksDBEngine() { return nullptr; }
}  // namespace bench

#endif
