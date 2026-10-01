#pragma once

#include <cstdint>
#include <memory>

#include "lsm/env.h"
#include "lsm/slice.h"
#include "lsm/status.h"
#include "table/table_options.h"

namespace lsm {

// Writes an SSTable. Keys must be internal keys, added in increasing order.
class TableBuilder {
 public:
  // file must outlive the builder. The caller syncs and closes it after
  // Finish().
  TableBuilder(const TableOptions& options, WritableFile* file);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: key is after every key added so far; Finish() and Abandon()
  // have not been called.
  void Add(Slice key, Slice value);

  // Writes the filter, metaindex, and index blocks and the footer.
  Status Finish();

  // Stops building; the file's contents are to be discarded.
  void Abandon();

  // The first error encountered, if any. Once non-OK, Add is a no-op.
  Status status() const;

  uint64_t NumEntries() const;
  // Bytes written so far; after Finish(), the size of the table.
  uint64_t FileSize() const;

 private:
  struct Rep;

  // Writes the pending data block, if any.
  void Flush();

  std::unique_ptr<Rep> rep_;
};

}  // namespace lsm
