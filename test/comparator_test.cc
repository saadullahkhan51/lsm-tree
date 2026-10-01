#include "lsm/comparator.h"

#include <string>

#include "gtest/gtest.h"

namespace lsm {

namespace {

std::string Separator(std::string start, const std::string& limit) {
  BytewiseComparator()->FindShortestSeparator(&start, limit);
  return start;
}

std::string Successor(std::string key) {
  BytewiseComparator()->FindShortSuccessor(&key);
  return key;
}

}  // namespace

TEST(BytewiseComparator, ComparesAsUnsignedBytes) {
  const Comparator* cmp = BytewiseComparator();
  ASSERT_LT(cmp->Compare("a", "b"), 0);
  ASSERT_GT(cmp->Compare("b", "a"), 0);
  ASSERT_EQ(cmp->Compare("abc", "abc"), 0);
  ASSERT_LT(cmp->Compare("ab", "abc"), 0);
  ASSERT_LT(cmp->Compare("", "a"), 0);
  // 0x80 and above must sort after ASCII, not before (signed char would).
  ASSERT_LT(cmp->Compare("\x7f", "\x80"), 0);
  ASSERT_LT(cmp->Compare("z", "\xff"), 0);
  // Embedded NUL bytes are data.
  ASSERT_LT(cmp->Compare(std::string("a\0b", 3), std::string("a\0c", 3)), 0);
  ASSERT_LT(cmp->Compare("a", std::string("a\0", 2)), 0);
}

TEST(BytewiseComparator, ShortestSeparator) {
  ASSERT_EQ("g", Separator("foo", "hello"));     // differ at byte 0, room to bump
  ASSERT_EQ("abd", Separator("abcxyz", "abq"));  // bump the first differing byte
  ASSERT_EQ("foo", Separator("foo", "foobar"));  // prefix: unchanged
  ASSERT_EQ("foobar", Separator("foobar", "foo"));
  ASSERT_EQ("abc", Separator("abc", "abd"));  // adjacent bytes: no room
  ASSERT_EQ("foo", Separator("foo", "foo"));
  ASSERT_EQ("a\xffz", Separator("a\xffz", "b"));  // adjacent at byte 0
}

TEST(BytewiseComparator, SeparatorStaysInRange) {
  const Comparator* cmp = BytewiseComparator();
  const std::string keys[] = {"", "a", "ab", "abc", "abd", "b", "b\xff", "c\x01", "\xff\xff"};
  for (const std::string& start : keys) {
    for (const std::string& limit : keys) {
      if (cmp->Compare(start, limit) >= 0) continue;
      const std::string sep = Separator(start, limit);
      ASSERT_LE(cmp->Compare(start, sep), 0) << start << " " << limit;
      ASSERT_LT(cmp->Compare(sep, limit), 0) << start << " " << limit;
      ASSERT_LE(sep.size(), start.size());
    }
  }
}

TEST(BytewiseComparator, ShortSuccessor) {
  ASSERT_EQ("g", Successor("foo"));
  ASSERT_EQ("\xff\xff", Successor("\xff\xff"));
  ASSERT_EQ("\xff" "b", Successor("\xff" "abc"));
  ASSERT_EQ("", Successor(""));
}

}  // namespace lsm
