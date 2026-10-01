#include "table/filter_block.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "gtest/gtest.h"
#include "util/coding.h"

namespace lsm {

namespace {

std::string Key(uint64_t i) {
  std::string key;
  PutFixed64(&key, i);
  return key;
}

// Builds a filter over keys [0, n) and measures how often it passes keys
// that were never added.
double MeasuredFalsePositiveRate(int bits_per_key, uint64_t n, uint64_t probes) {
  FilterBlockBuilder builder(bits_per_key);
  for (uint64_t i = 0; i < n; i++) builder.AddKey(Key(i));
  const Slice filter = builder.Finish();

  for (uint64_t i = 0; i < n; i += std::max<uint64_t>(1, n / 1000)) {
    EXPECT_TRUE(FilterKeyMayMatch(filter, Key(i))) << "false negative for key " << i;
  }

  uint64_t hits = 0;
  for (uint64_t i = 0; i < probes; i++) {
    if (FilterKeyMayMatch(filter, Key(i + 1'000'000'000))) hits++;
  }
  return static_cast<double>(hits) / static_cast<double>(probes);
}

// (1 - e^(-k/b))^k for b bits per key and k probes.
double TheoreticalFalsePositiveRate(int bits_per_key) {
  const int k = std::clamp(static_cast<int>(bits_per_key * 0.69), 1, 30);
  return std::pow(1.0 - std::exp(-static_cast<double>(k) / bits_per_key), k);
}

}  // namespace

TEST(FilterBlock, EmptyFilterMatchesNothing) {
  FilterBlockBuilder builder(10);
  const Slice filter = builder.Finish();
  ASSERT_FALSE(FilterKeyMayMatch(filter, "hello"));
  ASSERT_FALSE(FilterKeyMayMatch(filter, "world"));
}

TEST(FilterBlock, Small) {
  FilterBlockBuilder builder(10);
  builder.AddKey("hello");
  builder.AddKey("world");
  const Slice filter = builder.Finish();
  ASSERT_TRUE(FilterKeyMayMatch(filter, "hello"));
  ASSERT_TRUE(FilterKeyMayMatch(filter, "world"));
  ASSERT_FALSE(FilterKeyMayMatch(filter, "x"));
  ASSERT_FALSE(FilterKeyMayMatch(filter, "foo"));
}

TEST(FilterBlock, Layout) {
  // 10 bits per key: k = 6 probes, stored in the last byte. 100 keys need
  // 1000 bits, rounded up to whole bytes.
  FilterBlockBuilder builder(10);
  for (int i = 0; i < 100; i++) builder.AddKey(Key(i));
  const Slice filter = builder.Finish();
  ASSERT_EQ(125u + 1, filter.size());
  ASSERT_EQ(6, filter.back());
}

TEST(FilterBlock, RepeatedKeysAreCountedOnce) {
  // A table stores every version of a key consecutively.
  FilterBlockBuilder builder(10);
  for (int i = 0; i < 1000; i++) builder.AddKey("same");
  ASSERT_EQ(8u + 1, builder.Finish().size());  // the 64-bit minimum, plus k
}

TEST(FilterBlock, UnknownEncodingsMatchEverything) {
  ASSERT_TRUE(FilterKeyMayMatch("", "k"));
  ASSERT_TRUE(FilterKeyMayMatch("x", "k"));
  // k > 30 is reserved for future encodings.
  ASSERT_TRUE(FilterKeyMayMatch(std::string("\0\0\0\0\0\0\0\0\x1f", 9), "k"));
}

TEST(FilterBlock, NoFalseNegativesAtAnySize) {
  for (uint64_t n = 1; n <= 10000; n = n < 10 ? n + 1 : n * 3) {
    FilterBlockBuilder builder(10);
    for (uint64_t i = 0; i < n; i++) builder.AddKey(Key(i));
    const Slice filter = builder.Finish();
    ASSERT_LE(filter.size(), n * 10 / 8 + 40) << n;
    for (uint64_t i = 0; i < n; i++) {
      ASSERT_TRUE(FilterKeyMayMatch(filter, Key(i))) << "n=" << n << " key=" << i;
    }
  }
}

// The README's bloom table: measured against theoretical false positive
// rate across filter sizes, probing 1M absent keys each.
//
// Two filter populations. 20,000 keys is what one table holds (2 MiB of
// roughly 100-byte entries), so it is the rate lookups actually see. 1M
// keys in a single filter is far larger than any table's, and there the
// construction (every probe derived from one 32-bit hash and a rotation of
// it) runs about 1.5x above theory: the probe positions of a key are too
// correlated once the filter is much larger than the hash's rotation can
// spread them. Deriving the two hashes from independent halves of a 64-bit
// hash would close the gap.
TEST(FilterBlock, FalsePositiveRateTable) {
  constexpr uint64_t kProbes = 1'000'000;
  std::printf("\n  bits/key   k   theoretical   20K keys    1M keys\n");
  for (int bits_per_key : {4, 6, 8, 10, 12, 16}) {
    const double theoretical = TheoreticalFalsePositiveRate(bits_per_key);
    const double per_table = MeasuredFalsePositiveRate(bits_per_key, 20'000, kProbes);
    const double huge = MeasuredFalsePositiveRate(bits_per_key, 1'000'000, kProbes);
    std::printf("  %8d  %2d   %10.4f%%  %8.4f%%  %8.4f%%\n", bits_per_key,
                static_cast<int>(bits_per_key * 0.69), theoretical * 100, per_table * 100,
                huge * 100);
    EXPECT_LT(per_table, theoretical * 1.25 + 0.0003) << "bits_per_key=" << bits_per_key;
    EXPECT_GT(per_table, theoretical * 0.75) << "bits_per_key=" << bits_per_key;
    EXPECT_LT(huge, theoretical * 2.0) << "bits_per_key=" << bits_per_key;
  }
  std::printf("\n");
}

}  // namespace lsm
