#pragma once

#include <string>

#include "lsm/slice.h"

namespace lsm {

// Defines a total order over keys. Implementations must be thread-safe.
class Comparator {
 public:
  virtual ~Comparator() = default;

  // <0 if a < b, 0 if a == b, >0 if a > b.
  virtual int Compare(Slice a, Slice b) const = 0;

  // Persisted in the manifest; opening a database with a comparator of a
  // different name is refused.
  virtual const char* Name() const = 0;

  // If *start < limit, may change *start to a shorter string in
  // [*start, limit). Used to shrink index block separators.
  virtual void FindShortestSeparator(std::string* start, Slice limit) const = 0;

  // May change *key to a shorter string >= *key.
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Lexicographic order on unsigned bytes. The result is a process-lifetime
// singleton; do not delete it.
const Comparator* BytewiseComparator();

}  // namespace lsm
