#pragma once

#include <vector>

#include "lsm/comparator.h"
#include "lsm/iterator.h"

namespace lsm {

// An iterator over the union of the children's entries, in comparator
// order. Takes ownership of the children. If two children hold equal keys,
// both are yielded, in unspecified order.
//
// The result stops at the first error: if any child fails, the merged
// iterator becomes invalid and reports that child's status, rather than
// carrying on with the other children and silently omitting the failed
// child's entries.
Iterator* NewMergingIterator(const Comparator* comparator, std::vector<Iterator*> children);

}  // namespace lsm
