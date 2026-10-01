#pragma once

#include <functional>

#include "lsm/iterator.h"
#include "lsm/options.h"

namespace lsm {

// Turns an index value into an iterator over the block (or file) it names.
using BlockFunction = std::function<Iterator*(const ReadOptions&, Slice index_value)>;

// An iterator over the concatenation of the sequences that an index points
// at: index_iter yields values, block_function turns each into an iterator,
// and the result walks all their entries in order.
//
// Used for a table (index block -> data blocks) and for a sorted level
// (file list -> tables). Takes ownership of index_iter.
Iterator* NewTwoLevelIterator(Iterator* index_iter, BlockFunction block_function,
                              const ReadOptions& options);

}  // namespace lsm
