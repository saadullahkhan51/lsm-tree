#pragma once

#include <cstdint>
#include <string>

#include "lsm/env.h"
#include "lsm/iterator.h"
#include "lsm/options.h"
#include "table/table_options.h"
#include "util/cache.h"

namespace lsm {

class Table;

// Keeps recently used tables open: for each, the file descriptor and the
// parsed index and filter. Bounded by a number of entries, which is what
// keeps the process under its file descriptor limit. Thread-safe.
class TableCache {
 public:
  TableCache(const std::string& dbname, const TableOptions& options, Env* env, int entries);

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Returns an iterator over the given table file, which keeps the table
  // open for as long as the iterator lives. On failure the iterator
  // carries the error.
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size);

  // Looks up internal key k in the given table; see Table::InternalGet.
  Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, Slice k,
             void* arg, void (*handle_result)(void*, Slice, Slice));

  // Drops the cached entry for a file that is being deleted.
  void Evict(uint64_t file_number);

 private:
  Status FindTable(uint64_t file_number, uint64_t file_size, ShardedLRUCache::Handle** handle);

  const std::string dbname_;
  const TableOptions options_;
  Env* const env_;
  ShardedLRUCache cache_;
};

}  // namespace lsm
