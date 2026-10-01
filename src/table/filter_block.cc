#include "table/filter_block.h"

#include <algorithm>

#include "util/hash.h"

namespace lsm {

namespace {

uint32_t BloomHash(Slice key) { return Hash(key.data(), key.size(), 0xbc9f1d34); }

}  // namespace

FilterBlockBuilder::FilterBlockBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}

void FilterBlockBuilder::AddKey(Slice user_key) {
  const uint32_t h = BloomHash(user_key);
  // A table holds every version of a key back to back; count it once.
  if (hashes_.empty() || hashes_.back() != h) {
    hashes_.push_back(h);
  }
}

Slice FilterBlockBuilder::Finish() {
  // k = bits_per_key * ln(2) minimizes the false positive rate. Rounding
  // down trades a slightly higher rate for fewer probes.
  const int k = std::clamp(static_cast<int>(bits_per_key_ * 0.69), 1, 30);

  // A tiny filter would have a very high false positive rate; enforce a
  // minimum size.
  const size_t bits = std::max<size_t>(hashes_.size() * bits_per_key_, 64);
  const size_t bytes = (bits + 7) / 8;
  const uint32_t rounded_bits = static_cast<uint32_t>(bytes * 8);

  result_.assign(bytes, 0);
  for (uint32_t h : hashes_) {
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int j = 0; j < k; j++) {
      const uint32_t bitpos = h % rounded_bits;
      result_[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      h += delta;
    }
  }
  result_.push_back(static_cast<char>(k));
  return result_;
}

bool FilterKeyMayMatch(Slice filter, Slice user_key) {
  const size_t len = filter.size();
  if (len < 2) return true;  // not a filter this code wrote; cannot rule anything out

  const uint32_t bits = static_cast<uint32_t>((len - 1) * 8);
  const int k = static_cast<uint8_t>(filter[len - 1]);
  if (k > 30) {
    // Reserved for encodings this code does not know.
    return true;
  }

  uint32_t h = BloomHash(user_key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int j = 0; j < k; j++) {
    const uint32_t bitpos = h % bits;
    if ((filter[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

}  // namespace lsm
