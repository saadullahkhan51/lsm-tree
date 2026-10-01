#include "lsm/comparator.h"

#include <algorithm>
#include <cstdint>

namespace lsm {

namespace {

class BytewiseComparatorImpl : public Comparator {
 public:
  int Compare(Slice a, Slice b) const override { return a.compare(b); }

  const char* Name() const override { return "lsm.BytewiseComparator"; }

  void FindShortestSeparator(std::string* start, Slice limit) const override {
    const size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while (diff_index < min_length && (*start)[diff_index] == limit[diff_index]) {
      diff_index++;
    }
    if (diff_index >= min_length) {
      return;  // one string is a prefix of the other; nothing to shorten
    }
    const uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
    if (diff_byte < 0xff && diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
      (*start)[diff_index] = static_cast<char>(diff_byte + 1);
      start->resize(diff_index + 1);
    }
  }

  void FindShortSuccessor(std::string* key) const override {
    // Increment the first byte that can be incremented and drop the rest.
    for (size_t i = 0; i < key->size(); i++) {
      const uint8_t byte = static_cast<uint8_t>((*key)[i]);
      if (byte != 0xff) {
        (*key)[i] = static_cast<char>(byte + 1);
        key->resize(i + 1);
        return;
      }
    }
    // All 0xff: leave it alone.
  }
};

}  // namespace

const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl singleton;
  return &singleton;
}

}  // namespace lsm
