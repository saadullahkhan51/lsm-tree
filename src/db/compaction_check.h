#pragma once

#include <functional>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "lsm/comparator.h"
#include "lsm/iterator.h"
#include "lsm/status.h"

namespace lsm {

// Verifies that a compaction's output shows every reader what its input
// did. Debug builds run it after every compaction, before the result is
// installed.
//
// input and output iterate the same key range in internal key order.
// read_points are the sequence numbers at which reads can still happen:
// every live snapshot, and kMaxSequenceNumber for reads of the latest
// state. For each user key and each read point, the newest entry at or
// below the read point must be the same in the output as in the input,
// with one exception: a tombstone may be gone, provided
// key_may_exist_below(user_key) is false, that is, no level below the
// output could hold an older value for the tombstone to hide.
//
// It also checks that the output contains nothing the input did not.
//
// If the two differ, *violation describes the first difference; otherwise
// it is left empty. The return value says only whether the check could be
// carried out: it is the error, if any, from reading the iterators.
Status CheckCompactionOutput(const Comparator* user_comparator, Iterator* input, Iterator* output,
                             const std::vector<SequenceNumber>& read_points,
                             const std::function<bool(Slice user_key)>& key_may_exist_below,
                             std::string* violation);

}  // namespace lsm
