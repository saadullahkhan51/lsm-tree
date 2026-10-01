#pragma once

#include "db/dbformat.h"
#include "lsm/comparator.h"
#include "lsm/iterator.h"

namespace lsm {

// Turns an iterator over internal keys into the view a user sees as of
// `sequence`: for each user key, the newest entry at or below that
// sequence, skipped entirely if it is a tombstone. Yields user keys.
//
// Takes ownership of internal_iter.
Iterator* NewDBIterator(const Comparator* user_comparator, Iterator* internal_iter,
                        SequenceNumber sequence);

}  // namespace lsm
