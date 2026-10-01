#pragma once

#include <cstddef>

#include "lsm/comparator.h"

namespace lsm {

class ShardedLRUCache;

// What the table layer needs from Options. The database builds one of
// these with its internal-key comparator and its block cache.
struct TableOptions {
  // Orders the keys stored in tables. Tables hold internal keys.
  const Comparator* comparator = nullptr;
  size_t block_size = 4096;
  int block_restart_interval = 16;
  // 0 disables the bloom filter.
  int bloom_bits_per_key = 10;
  // Null disables block caching.
  ShardedLRUCache* block_cache = nullptr;
};

}  // namespace lsm
