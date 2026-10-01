#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsm/slice.h"

namespace lsm {

// Builds a block: entries with prefix-compressed keys, followed by an array
// of restart points and its length.
//
//   entry := varint32 shared | varint32 non_shared | varint32 value_len |
//            key[shared..] | value
//   block := entry* | fixed32 restart_offset* | fixed32 num_restarts
//
// Each key drops the prefix it shares with the key before it. Every
// restart_interval entries the full key is stored (shared = 0) and the
// entry's offset is recorded as a restart point, which is what lets a
// reader binary-search the block.
class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Clears the contents as if freshly constructed.
  void Reset();

  // REQUIRES: Finish() not called since the last Reset(); key is larger
  // than any key added so far.
  void Add(Slice key, Slice value);

  // Appends the restart array and returns the finished block, which stays
  // valid until Reset().
  Slice Finish();

  // Size the block would have if finished now.
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;  // entries since the last restart point
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace lsm
