#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsm/slice.h"

namespace lsm {

// The filter block of a table: one bloom filter over every user key in it.
//
//   filter := bit array | k (1 byte, the number of probes)
//
// The k probe positions for a key come from a single 32-bit hash by double
// hashing (Kirsch-Mitzenmacher): probe i is at h + i * delta, where delta
// is h rotated right by 17 bits.

// Name of the metaindex entry that points at the filter block. A reader
// that does not recognize the name ignores the filter.
inline constexpr char kFilterMetaKey[] = "filter.lsm.BloomFilter";

class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(int bits_per_key);

  void AddKey(Slice user_key);

  // The finished filter, valid for the lifetime of the builder.
  Slice Finish();

 private:
  const int bits_per_key_;
  std::vector<uint32_t> hashes_;
  std::string result_;
};

// False only if user_key was definitely not added to the filter.
bool FilterKeyMayMatch(Slice filter, Slice user_key);

}  // namespace lsm
