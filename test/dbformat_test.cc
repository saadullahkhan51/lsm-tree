#include "db/dbformat.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "gtest/gtest.h"

namespace lsm {

namespace {

std::string IKey(Slice user_key, uint64_t seq, ValueType type) {
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey{user_key, seq, type});
  return encoded;
}

std::string Shorten(const std::string& s, const std::string& limit) {
  std::string result = s;
  InternalKeyComparator(BytewiseComparator()).FindShortestSeparator(&result, limit);
  return result;
}

std::string ShortSuccessor(const std::string& s) {
  std::string result = s;
  InternalKeyComparator(BytewiseComparator()).FindShortSuccessor(&result);
  return result;
}

int Sign(int x) { return (x > 0) - (x < 0); }

const std::vector<std::string> kUserKeys = {
    "", "a", std::string("a\0", 2), "ab", "b", "hello", "\x7f", "\x80", "\xff", "\xff\xff",
    std::string(300, 'x'),
};

const std::vector<uint64_t> kSequences = {
    0,
    1,
    2,
    (1ull << 8) - 1,
    1ull << 8,
    (1ull << 8) + 1,
    (1ull << 16) - 1,
    1ull << 16,
    (1ull << 32) - 1,
    1ull << 32,
    (1ull << 32) + 1,
    kMaxSequenceNumber - 1,
    kMaxSequenceNumber,
};

}  // namespace

TEST(InternalKey, EncodeDecodeRoundTrip) {
  for (const std::string& user_key : kUserKeys) {
    for (uint64_t seq : kSequences) {
      for (ValueType type : {kTypeDeletion, kTypeValue}) {
        const std::string encoded = IKey(user_key, seq, type);
        ASSERT_EQ(user_key.size() + 8, encoded.size());
        ASSERT_EQ(user_key, ExtractUserKey(encoded));

        ParsedInternalKey decoded;
        ASSERT_TRUE(ParseInternalKey(encoded, &decoded));
        ASSERT_EQ(user_key, decoded.user_key);
        ASSERT_EQ(seq, decoded.sequence);
        ASSERT_EQ(type, decoded.type);
      }
    }
  }
}

TEST(InternalKey, TrailerLayout) {
  // (sequence << 8) | type, little-endian: the type is the first trailer byte.
  const std::string encoded = IKey("k", 0x0102030405, kTypeValue);
  ASSERT_EQ(std::string("k\x01\x05\x04\x03\x02\x01\x00\x00", 9), encoded);
}

TEST(InternalKey, ParseRejectsMalformed) {
  ParsedInternalKey decoded;
  ASSERT_FALSE(ParseInternalKey("", &decoded));
  ASSERT_FALSE(ParseInternalKey("1234567", &decoded));  // shorter than a trailer

  std::string bad_type = IKey("k", 5, kTypeValue);
  bad_type[1] = 2;  // type byte beyond kTypeValue
  ASSERT_FALSE(ParseInternalKey(bad_type, &decoded));
}

// The whole engine leans on this order: user key ascending, then sequence
// descending, then type descending. Checked over every pair.
TEST(InternalKeyComparator, OrderingExhaustive) {
  struct Entry {
    std::string user_key;
    uint64_t seq;
    ValueType type;
    std::string encoded;
  };
  std::vector<Entry> entries;
  for (const std::string& user_key : kUserKeys) {
    for (uint64_t seq : kSequences) {
      for (ValueType type : {kTypeDeletion, kTypeValue}) {
        entries.push_back({user_key, seq, type, IKey(user_key, seq, type)});
      }
    }
  }

  const InternalKeyComparator icmp(BytewiseComparator());
  auto expected = [](const Entry& a, const Entry& b) {
    if (int r = a.user_key.compare(b.user_key); r != 0) return Sign(r);
    if (a.seq != b.seq) return a.seq > b.seq ? -1 : +1;
    if (a.type != b.type) return a.type > b.type ? -1 : +1;
    return 0;
  };
  for (const Entry& a : entries) {
    for (const Entry& b : entries) {
      ASSERT_EQ(expected(a, b), Sign(icmp.Compare(a.encoded, b.encoded)))
          << "a=(" << a.user_key << "," << a.seq << "," << int{a.type} << ") b=(" << b.user_key
          << "," << b.seq << "," << int{b.type} << ")";
    }
  }
}

TEST(InternalKeyComparator, NewestFirstAndValueBeforeTombstone) {
  const InternalKeyComparator icmp(BytewiseComparator());
  ASSERT_LT(icmp.Compare(IKey("k", 5, kTypeValue), IKey("k", 4, kTypeValue)), 0);
  ASSERT_LT(icmp.Compare(IKey("k", 5, kTypeValue), IKey("k", 5, kTypeDeletion)), 0);
  ASSERT_LT(icmp.Compare(IKey("k", 5, kTypeDeletion), IKey("k", 4, kTypeValue)), 0);
  // The user key dominates the sequence.
  ASSERT_LT(icmp.Compare(IKey("a", 1, kTypeValue), IKey("b", 100, kTypeValue)), 0);
}

TEST(InternalKeyComparator, LookupKeyLandsOnNewestVisibleVersion) {
  const InternalKeyComparator icmp(BytewiseComparator());
  std::vector<std::string> keys = {
      IKey("j", 20, kTypeValue),   IKey("k", 10, kTypeValue), IKey("k", 7, kTypeDeletion),
      IKey("k", 3, kTypeValue),    IKey("l", 1, kTypeValue),
  };
  std::sort(keys.begin(), keys.end(),
            [&](const std::string& a, const std::string& b) { return icmp.Compare(a, b) < 0; });

  // First entry >= the lookup key, as a memtable or block seek would find.
  auto seek = [&](Slice user_key, SequenceNumber snapshot) {
    const LookupKey lookup(user_key, snapshot);
    auto it = std::lower_bound(keys.begin(), keys.end(), lookup.internal_key(),
                               [&](const std::string& a, Slice b) { return icmp.Compare(a, b) < 0; });
    ParsedInternalKey parsed;
    EXPECT_TRUE(it != keys.end() && ParseInternalKey(*it, &parsed));
    return std::tuple(std::string(parsed.user_key), parsed.sequence, parsed.type);
  };

  ASSERT_EQ(std::tuple(std::string("k"), uint64_t{10}, kTypeValue), seek("k", kMaxSequenceNumber));
  ASSERT_EQ(std::tuple(std::string("k"), uint64_t{10}, kTypeValue), seek("k", 10));
  ASSERT_EQ(std::tuple(std::string("k"), uint64_t{7}, kTypeDeletion), seek("k", 9));
  // A tombstone at exactly the snapshot sequence is visible to it.
  ASSERT_EQ(std::tuple(std::string("k"), uint64_t{7}, kTypeDeletion), seek("k", 7));
  ASSERT_EQ(std::tuple(std::string("k"), uint64_t{3}, kTypeValue), seek("k", 6));
  // Nothing for "k" is visible at 2; the seek falls through to the next key.
  ASSERT_EQ(std::tuple(std::string("l"), uint64_t{1}, kTypeValue), seek("k", 2));
}

TEST(InternalKeyComparator, ShortestSeparator) {
  // Same user key: never shortened, whatever the trailers.
  ASSERT_EQ(IKey("foo", 100, kTypeValue),
            Shorten(IKey("foo", 100, kTypeValue), IKey("foo", 99, kTypeValue)));
  ASSERT_EQ(IKey("foo", 100, kTypeValue),
            Shorten(IKey("foo", 100, kTypeValue), IKey("foo", 101, kTypeValue)));
  ASSERT_EQ(IKey("foo", 100, kTypeValue),
            Shorten(IKey("foo", 100, kTypeValue), IKey("foo", 100, kTypeDeletion)));

  // Misordered user keys: unchanged.
  ASSERT_EQ(IKey("foo", 100, kTypeValue),
            Shorten(IKey("foo", 100, kTypeValue), IKey("bar", 99, kTypeValue)));

  // Different user keys with room between them.
  ASSERT_EQ(IKey("g", kMaxSequenceNumber, kValueTypeForSeek),
            Shorten(IKey("foo", 100, kTypeValue), IKey("hello", 200, kTypeValue)));

  // One user key is a prefix of the other: unchanged.
  ASSERT_EQ(IKey("foo", 100, kTypeValue),
            Shorten(IKey("foo", 100, kTypeValue), IKey("foobar", 200, kTypeValue)));
  ASSERT_EQ(IKey("foobar", 100, kTypeValue),
            Shorten(IKey("foobar", 100, kTypeValue), IKey("foo", 200, kTypeValue)));
}

TEST(InternalKeyComparator, SeparatorStaysInRange) {
  const InternalKeyComparator icmp(BytewiseComparator());
  for (const std::string& a : kUserKeys) {
    for (const std::string& b : kUserKeys) {
      const std::string start = IKey(a, 100, kTypeValue);
      const std::string limit = IKey(b, 200, kTypeValue);
      if (icmp.Compare(start, limit) >= 0) continue;
      const std::string sep = Shorten(start, limit);
      ASSERT_LE(icmp.Compare(start, sep), 0);
      ASSERT_LT(icmp.Compare(sep, limit), 0);
    }
  }
}

TEST(InternalKeyComparator, ShortSuccessor) {
  ASSERT_EQ(IKey("g", kMaxSequenceNumber, kValueTypeForSeek),
            ShortSuccessor(IKey("foo", 100, kTypeValue)));
  ASSERT_EQ(IKey("\xff\xff", 100, kTypeValue), ShortSuccessor(IKey("\xff\xff", 100, kTypeValue)));
}

TEST(LookupKey, KeyForms) {
  for (const std::string& user_key : kUserKeys) {  // includes one past the inline buffer
    const LookupKey lookup(user_key, 42);
    const std::string internal = IKey(user_key, 42, kValueTypeForSeek);
    ASSERT_EQ(user_key, lookup.user_key());
    ASSERT_EQ(internal, lookup.internal_key());

    std::string memtable_key;
    PutLengthPrefixedSlice(&memtable_key, internal);
    ASSERT_EQ(memtable_key, lookup.memtable_key());
  }
}

TEST(InternalKeyClass, EncodeAndDecode) {
  const InternalKey key("foo", 7, kTypeDeletion);
  ASSERT_EQ(IKey("foo", 7, kTypeDeletion), key.Encode());
  ASSERT_EQ("foo", key.user_key());

  InternalKey copy;
  copy.DecodeFrom(key.Encode());
  ASSERT_EQ(key.Encode(), copy.Encode());
}

}  // namespace lsm
