#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bench {

// The settings every engine is opened with, so that a comparison changes
// only the engine.
struct EngineConfig {
  std::string path;
  size_t write_buffer_size = 64 << 20;
  size_t block_cache_size = 8 << 20;
  size_t block_size = 4096;
  int bloom_bits_per_key = 10;
  // Level shape. Honored by lsm and LevelDB (file size only); RocksDB keeps
  // its own defaults.
  size_t max_file_size = 2 << 20;
  uint64_t max_bytes_for_level_base = 10 << 20;
  int level_size_multiplier = 10;
  // Verify block checksums on every read. lsm always does. RocksDB does by
  // default. LevelDB does not by default, and that is a large part of its
  // read speed, so the comparison turns it on and reports both.
  bool verify_checksums = true;
  // lsm only: turn group commit off, to measure what it buys.
  bool group_commit = true;
};

// A forward/backward cursor, as each engine's iterator provides.
class Cursor {
 public:
  virtual ~Cursor() = default;
  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  virtual void Prev() = 0;
  virtual bool Valid() const = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
};

// The operations db_bench drives. Compression is off in every engine.
class Engine {
 public:
  virtual ~Engine() = default;

  virtual const char* name() const = 0;
  // Returns an error message, or empty on success.
  virtual std::string Open(const EngineConfig& config) = 0;
  virtual void Close() = 0;

  virtual bool Put(std::string_view key, std::string_view value, bool sync) = 0;
  virtual bool Delete(std::string_view key, bool sync) = 0;
  // Returns false on error; *found says whether the key exists.
  virtual bool Get(std::string_view key, std::string* value, bool* found) = 0;
  virtual std::unique_ptr<Cursor> NewCursor() = 0;

  // Compacts the whole key range.
  virtual void CompactAll() = 0;
  // Blocks until no flush or compaction is pending, where the engine can
  // tell.
  virtual void WaitForBackground() {}

  // Free-form statistics for the report.
  virtual std::string Stats() = 0;
  // A named counter, where the engine has it. lsm exposes its properties.
  virtual bool Counter(const std::string& /*name*/, std::string* /*value*/) { return false; }
};

std::unique_ptr<Engine> NewLsmEngine();
// Null if db_bench was built without the library.
std::unique_ptr<Engine> NewLevelDBEngine();
std::unique_ptr<Engine> NewRocksDBEngine();

}  // namespace bench
