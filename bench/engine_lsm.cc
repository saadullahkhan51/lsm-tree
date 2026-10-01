#include "db/db_impl.h"
#include "engine.h"
#include "lsm/db.h"

namespace bench {

namespace {

class LsmCursor : public Cursor {
 public:
  explicit LsmCursor(lsm::Iterator* iter) : iter_(iter) {}
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void SeekToLast() override { iter_->SeekToLast(); }
  void Seek(std::string_view target) override { iter_->Seek(target); }
  void Next() override { iter_->Next(); }
  void Prev() override { iter_->Prev(); }
  bool Valid() const override { return iter_->Valid(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }

 private:
  std::unique_ptr<lsm::Iterator> iter_;
};

class LsmEngine : public Engine {
 public:
  ~LsmEngine() override { Close(); }

  const char* name() const override { return "lsm"; }

  std::string Open(const EngineConfig& config) override {
    lsm::Options options;
    options.write_buffer_size = config.write_buffer_size;
    options.block_cache_size = config.block_cache_size;
    options.block_size = config.block_size;
    options.bloom_bits_per_key = config.bloom_bits_per_key;
    options.max_file_size = config.max_file_size;
    options.max_bytes_for_level_base = config.max_bytes_for_level_base;
    options.level_size_multiplier = config.level_size_multiplier;
    lsm::DB* db = nullptr;
    lsm::Status s = lsm::DB::Open(options, config.path, &db);
    if (!s.ok()) return s.ToString();
    db_.reset(db);
    static_cast<lsm::DBImpl*>(db)->TEST_SetGroupCommit(config.group_commit);
    return "";
  }

  void Close() override { db_.reset(); }

  bool Put(std::string_view key, std::string_view value, bool sync) override {
    lsm::WriteOptions options;
    options.sync = sync;
    return db_->Put(options, key, value).ok();
  }

  bool Delete(std::string_view key, bool sync) override {
    lsm::WriteOptions options;
    options.sync = sync;
    return db_->Delete(options, key).ok();
  }

  bool Get(std::string_view key, std::string* value, bool* found) override {
    lsm::Status s = db_->Get(lsm::ReadOptions(), key, value);
    *found = s.ok();
    return s.ok() || s.IsNotFound();
  }

  std::unique_ptr<Cursor> NewCursor() override {
    return std::make_unique<LsmCursor>(db_->NewIterator(lsm::ReadOptions()));
  }

  void CompactAll() override { (void)db_->CompactRange(nullptr, nullptr); }

  void WaitForBackground() override {
    (void)static_cast<lsm::DBImpl*>(db_.get())->TEST_WaitForBackgroundWork();
  }

  std::string Stats() override {
    std::string stats;
    db_->GetProperty("lsm.stats", &stats);
    return stats;
  }

  bool Counter(const std::string& name, std::string* value) override {
    return db_->GetProperty(name, value);
  }

 private:
  std::unique_ptr<lsm::DB> db_;
};

}  // namespace

std::unique_ptr<Engine> NewLsmEngine() { return std::make_unique<LsmEngine>(); }

}  // namespace bench
