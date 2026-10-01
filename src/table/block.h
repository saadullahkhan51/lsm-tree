#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lsm/comparator.h"
#include "lsm/iterator.h"
#include "table/format.h"

namespace lsm {

// An immutable, parsed block. See block_builder.h for the layout.
class Block {
 public:
  // Takes ownership of the bytes.
  explicit Block(BlockContents contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return size_; }

  // The block must outlive the iterator. A malformed block yields an
  // iterator whose status() is Corruption.
  Iterator* NewIterator(const Comparator* comparator) const;

 private:
  class Iter;

  uint32_t NumRestarts() const;

  std::unique_ptr<char[]> data_;
  size_t size_;              // 0 marks a block too malformed to iterate
  uint32_t restart_offset_ = 0;  // offset of the restart array
};

}  // namespace lsm
