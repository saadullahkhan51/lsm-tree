#pragma once

#include <cstdint>
#include <memory>

#include "lsm/env.h"
#include "lsm/iterator.h"
#include "lsm/options.h"
#include "table/table_options.h"

namespace lsm {

// An open, immutable SSTable. Safe for concurrent use.
class Table {
 public:
  // Opens the table stored in file, whose length is file_size. Reads the
  // footer, the index block, and the filter; data blocks are read on
  // demand. file_number keys this table's blocks in the block cache.
  static Status Open(const TableOptions& options, std::unique_ptr<RandomAccessFile> file,
                     uint64_t file_number, uint64_t file_size, std::unique_ptr<Table>* table);

  ~Table();

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // The table must outlive the iterator.
  Iterator* NewIterator(const ReadOptions& options) const;

  // Finds the first entry at or after key and, if there is one, calls
  // handle_result(arg, entry_key, entry_value). Does not call it if the
  // bloom filter rules out key's user key. key is an internal key.
  Status InternalGet(const ReadOptions& options, Slice key, void* arg,
                     void (*handle_result)(void* arg, Slice key, Slice value)) const;

 private:
  struct Rep;

  explicit Table(std::unique_ptr<Rep> rep);

  // Returns an iterator over the data block that index_value points at.
  Iterator* BlockReader(const ReadOptions& options, Slice index_value) const;

  const std::unique_ptr<Rep> rep_;
};

}  // namespace lsm
