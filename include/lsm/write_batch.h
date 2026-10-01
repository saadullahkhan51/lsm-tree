#pragma once

#include <string>

#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// A set of updates applied atomically: after a crash either all of a batch
// is recovered or none of it. Updates apply in the order they were added,
// so a later Put or Delete of a key overrides an earlier one.
//
// Not thread-safe for concurrent modification.
class WriteBatch {
 public:
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(Slice key, Slice value) = 0;
    virtual void Delete(Slice key) = 0;
  };

  WriteBatch();

  void Put(Slice key, Slice value);
  void Delete(Slice key);
  void Clear();

  // Bytes this batch will occupy in the write-ahead log.
  size_t ApproximateSize() const { return rep_.size(); }

  // Appends the updates in source to this batch.
  void Append(const WriteBatch& source);

  // Calls handler for each update in order.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  // fixed64 sequence | fixed32 count | record*
  // record := kTypeValue    varstring(key) varstring(value)
  //         | kTypeDeletion varstring(key)
  std::string rep_;
};

}  // namespace lsm
