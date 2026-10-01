#pragma once

#include <cstddef>
#include <cstdint>

#include "lsm/comparator.h"
#include "lsm/env.h"

namespace lsm {

class Snapshot;

// Stored in every block trailer. Compression is out of scope for v1; the
// byte is reserved and any other value is reported as corruption.
enum CompressionType : uint8_t { kNoCompression = 0 };

struct Options {
  // Memtable size at which it is frozen and flushed to an SSTable.
  size_t write_buffer_size = 4 << 20;
  int max_open_files = 1000;
  // Approximate uncompressed size of an SSTable data block.
  size_t block_size = 4096;
  int block_restart_interval = 16;
  size_t block_cache_size = 8 << 20;
  // 0 disables bloom filters.
  int bloom_bits_per_key = 10;
  CompressionType compression = kNoCompression;
  // Accepted for LevelDB compatibility and has no effect: every block read
  // is checksummed and every error found during recovery is fatal, whether
  // or not this is set.
  bool paranoid_checks = false;

  // --- Level shape. These set the write/space amplification tradeoff. ---

  // Compaction starts a new output table once the current one reaches this.
  size_t max_file_size = 2 << 20;
  // Target total size of level 1.
  uint64_t max_bytes_for_level_base = 10 << 20;
  // Each level's target is this many times the one above it.
  int level_size_multiplier = 10;

  const Comparator* comparator = BytewiseComparator();
  Env* env = Env::Default();
};

struct ReadOptions {
  // Read as of this snapshot; null means the latest state.
  const Snapshot* snapshot = nullptr;
  // Whether blocks read for this operation are added to the block cache.
  bool fill_cache = true;
};

struct WriteOptions {
  // true: the write is on stable storage before it is acknowledged.
  // false: the write has reached the OS, so it survives a process crash but
  // may be lost on power failure.
  bool sync = false;
};

}  // namespace lsm
