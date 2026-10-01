#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "gtest/gtest.h"
#include "lsm/iterator.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/format.h"
#include "table/table.h"
#include "table/table_builder.h"
#include "testutil.h"
#include "util/cache.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

namespace {

const InternalKeyComparator kInternalComparator(BytewiseComparator());

struct InternalLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return kInternalComparator.Compare(a, b) < 0;
  }
};

// The reference: internal key -> value, in internal key order.
using Model = std::map<std::string, std::string, InternalLess>;

std::string IKey(Slice user_key, SequenceNumber seq, ValueType type = kTypeValue) {
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey{user_key, seq, type});
  return encoded;
}

std::string RandomString(std::mt19937* rnd, size_t len, const std::string& alphabet) {
  std::string s;
  for (size_t i = 0; i < len; i++) s.push_back(alphabet[(*rnd)() % alphabet.size()]);
  return s;
}

// User keys from a small alphabet, so that many share prefixes and some
// repeat with different sequence numbers.
std::string RandomUserKey(std::mt19937* rnd) {
  static const std::string kAlphabets[] = {"ab", "abcdef", std::string("\0\x01\xfe\xff", 4)};
  return RandomString(rnd, (*rnd)() % 12, kAlphabets[(*rnd)() % 3]);
}

Model RandomModel(std::mt19937* rnd, int entries) {
  Model model;
  for (int i = 0; i < entries; i++) {
    const std::string key = IKey(RandomUserKey(rnd), 1 + (*rnd)() % 100,
                                 (*rnd)() % 4 == 0 ? kTypeDeletion : kTypeValue);
    model[key] = RandomString(rnd, (*rnd)() % 5 == 0 ? (*rnd)() % 300 : (*rnd)() % 20, "xyz");
  }
  return model;
}

// One kind of container holding a Model, iterated through the Iterator
// interface.
class Constructor {
 public:
  virtual ~Constructor() = default;
  virtual void Build(const TableOptions& options, const Model& model) = 0;
  virtual Iterator* NewIterator() const = 0;
};

class BlockConstructor : public Constructor {
 public:
  void Build(const TableOptions& options, const Model& model) override {
    BlockBuilder builder(options.block_restart_interval);
    for (const auto& [key, value] : model) builder.Add(key, value);
    const Slice data = builder.Finish();
    BlockContents contents;
    contents.data = std::make_unique<char[]>(data.size());
    std::memcpy(contents.data.get(), data.data(), data.size());
    contents.size = data.size();
    block_ = std::make_unique<Block>(std::move(contents));
  }
  Iterator* NewIterator() const override { return block_->NewIterator(&kInternalComparator); }

 private:
  std::unique_ptr<Block> block_;
};

// Builds a table into a string. Returns the file's bytes.
std::string BuildTableFile(const TableOptions& options, const Model& model) {
  test::StringSink sink;
  TableBuilder builder(options, &sink);
  for (const auto& [key, value] : model) builder.Add(key, value);
  EXPECT_OK(builder.Finish());
  EXPECT_EQ(model.size(), builder.NumEntries());
  EXPECT_EQ(sink.contents_.size(), builder.FileSize());
  return sink.contents_;
}

Status OpenTableFile(const TableOptions& options, const std::string& file,
                     std::unique_ptr<Table>* table) {
  return Table::Open(options, std::make_unique<test::StringSource>(file), /*file_number=*/1,
                     file.size(), table);
}

class TableConstructor : public Constructor {
 public:
  void Build(const TableOptions& options, const Model& model) override {
    ASSERT_OK(OpenTableFile(options, BuildTableFile(options, model), &table_));
  }
  Iterator* NewIterator() const override { return table_->NewIterator(ReadOptions()); }

 private:
  std::unique_ptr<Table> table_;
};

class MemTableConstructor : public Constructor {
 public:
  ~MemTableConstructor() override {
    if (memtable_ != nullptr) memtable_->Unref();
  }
  void Build(const TableOptions&, const Model& model) override {
    memtable_ = new MemTable(kInternalComparator);
    memtable_->Ref();
    for (const auto& [key, value] : model) {
      ParsedInternalKey parsed;
      ASSERT_TRUE(ParseInternalKey(key, &parsed));
      memtable_->Add(parsed.sequence, parsed.type, parsed.user_key, value);
    }
  }
  Iterator* NewIterator() const override { return memtable_->NewIterator(); }

 private:
  MemTable* memtable_ = nullptr;
};

enum class Kind { kBlock, kTable, kMemTable };

struct HarnessParam {
  Kind kind;
  int restart_interval;
  size_t block_size;
};

std::string ParamName(const testing::TestParamInfo<HarnessParam>& info) {
  const char* kind = info.param.kind == Kind::kBlock   ? "Block"
                     : info.param.kind == Kind::kTable ? "Table"
                                                       : "MemTable";
  return std::string(kind) + "_restart" + std::to_string(info.param.restart_interval) + "_block" +
         std::to_string(info.param.block_size);
}

std::string ToString(const Iterator* iter) {
  if (!iter->Valid()) return "END";
  return std::string(iter->key()) + "->" + std::string(iter->value());
}

std::string ToString(const Model& model, Model::const_iterator it) {
  if (it == model.end()) return "END";
  return it->first + "->" + it->second;
}

}  // namespace

// Every container is checked against the same model with the same
// operations: full scans both ways and random walks mixing seeks and steps.
class IteratorHarness : public testing::TestWithParam<HarnessParam> {
 protected:
  void Build(const Model& model) {
    switch (GetParam().kind) {
      case Kind::kBlock:
        constructor_ = std::make_unique<BlockConstructor>();
        break;
      case Kind::kTable:
        constructor_ = std::make_unique<TableConstructor>();
        break;
      case Kind::kMemTable:
        constructor_ = std::make_unique<MemTableConstructor>();
        break;
    }
    options_.comparator = &kInternalComparator;
    options_.block_restart_interval = GetParam().restart_interval;
    options_.block_size = GetParam().block_size;
    constructor_->Build(options_, model);
  }

  void TestForwardScan(const Model& model) {
    std::unique_ptr<Iterator> iter(constructor_->NewIterator());
    ASSERT_FALSE(iter->Valid());
    iter->SeekToFirst();
    for (auto it = model.begin(); it != model.end(); ++it) {
      ASSERT_EQ(ToString(model, it), ToString(iter.get()));
      iter->Next();
    }
    ASSERT_FALSE(iter->Valid());
    ASSERT_OK(iter->status());
  }

  void TestBackwardScan(const Model& model) {
    std::unique_ptr<Iterator> iter(constructor_->NewIterator());
    iter->SeekToLast();
    for (auto it = model.rbegin(); it != model.rend(); ++it) {
      ASSERT_EQ(it->first + "->" + it->second, ToString(iter.get()));
      iter->Prev();
    }
    ASSERT_FALSE(iter->Valid());
    ASSERT_OK(iter->status());
  }

  void TestRandomAccess(std::mt19937* rnd, const Model& model) {
    std::unique_ptr<Iterator> iter(constructor_->NewIterator());
    auto model_it = model.end();
    for (int step = 0; step < 300; step++) {
      switch ((*rnd)() % 5) {
        case 0:
          if (iter->Valid()) {
            iter->Next();
            ++model_it;
          }
          break;
        case 1:
          iter->SeekToFirst();
          model_it = model.begin();
          break;
        case 2: {
          const std::string target = PickSeekTarget(rnd, model);
          iter->Seek(target);
          model_it = model.lower_bound(target);
          break;
        }
        case 3:
          if (iter->Valid()) {
            iter->Prev();
            model_it = model_it == model.begin() ? model.end() : std::prev(model_it);
          }
          break;
        case 4:
          iter->SeekToLast();
          model_it = model.empty() ? model.end() : std::prev(model.end());
          break;
      }
      ASSERT_EQ(ToString(model, model_it), ToString(iter.get())) << "step " << step;
    }
    ASSERT_OK(iter->status());
  }

  // An existing key, or one near an existing key, or an arbitrary one.
  static std::string PickSeekTarget(std::mt19937* rnd, const Model& model) {
    if (model.empty() || (*rnd)() % 4 == 0) {
      return IKey(RandomUserKey(rnd), (*rnd)() % 200);
    }
    auto it = model.begin();
    std::advance(it, (*rnd)() % model.size());
    ParsedInternalKey parsed;
    EXPECT_TRUE(ParseInternalKey(it->first, &parsed));
    switch ((*rnd)() % 4) {
      case 0:
        return it->first;
      case 1:  // same user key, newer than any stored version
        return IKey(parsed.user_key, 150);
      case 2:  // same user key, older than any stored version
        return IKey(parsed.user_key, 0, kTypeDeletion);
      default:  // a user key just after it
        return IKey(std::string(parsed.user_key) + '\0', parsed.sequence);
    }
  }

  TableOptions options_;
  std::unique_ptr<Constructor> constructor_;
};

TEST_P(IteratorHarness, Empty) {
  const Model model;
  Build(model);
  std::mt19937 rnd(1);
  TestForwardScan(model);
  TestBackwardScan(model);
  TestRandomAccess(&rnd, model);
}

TEST_P(IteratorHarness, SingleEntry) {
  const Model model = {{IKey("k", 7), "v"}};
  Build(model);
  std::mt19937 rnd(2);
  TestForwardScan(model);
  TestBackwardScan(model);
  TestRandomAccess(&rnd, model);
}

TEST_P(IteratorHarness, EmptyUserKeyAndEmptyValue) {
  const Model model = {{IKey("", 3), ""}, {IKey("", 2, kTypeDeletion), ""}, {IKey("a", 1), ""}};
  Build(model);
  std::mt19937 rnd(3);
  TestForwardScan(model);
  TestBackwardScan(model);
  TestRandomAccess(&rnd, model);
}

TEST_P(IteratorHarness, RandomizedAgainstModel) {
  std::mt19937 rnd(GetParam().restart_interval * 31 + GetParam().block_size);
  // Sizes around every restart-interval and block boundary, then larger.
  for (int entries = 0; entries < 2000; entries += entries < 50 ? 1 : 200) {
    SCOPED_TRACE("entries=" + std::to_string(entries));
    const Model model = RandomModel(&rnd, entries);
    Build(model);
    TestForwardScan(model);
    TestBackwardScan(model);
    TestRandomAccess(&rnd, model);
    if (HasFatalFailure()) return;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Containers, IteratorHarness,
    testing::Values(HarnessParam{Kind::kBlock, 1, 0}, HarnessParam{Kind::kBlock, 16, 0},
                    HarnessParam{Kind::kBlock, 1024, 0},  // one restart point for the whole block
                    HarnessParam{Kind::kTable, 1, 256}, HarnessParam{Kind::kTable, 16, 256},
                    HarnessParam{Kind::kTable, 16, 4096},
                    HarnessParam{Kind::kTable, 16, 1 << 20},  // everything in one data block
                    HarnessParam{Kind::kMemTable, 0, 0}),
    ParamName);

// --- Blocks ---

TEST(Block, MalformedContentsGiveAnErrorIterator) {
  for (const std::string& bytes :
       {std::string(""), std::string("abc"),                 // shorter than the restart count
        std::string("\xff\xff\xff\x7f", 4),                  // count far larger than the block
        std::string("\x00\x00\x00\x00\x02\x00\x00\x00", 8),  // 2 restarts, room for 1
       }) {
    BlockContents contents;
    contents.data = std::make_unique<char[]>(bytes.size());
    std::memcpy(contents.data.get(), bytes.data(), bytes.size());
    contents.size = bytes.size();
    const Block block(std::move(contents));
    std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
    iter->SeekToFirst();
    ASSERT_FALSE(iter->Valid());
    ASSERT_TRUE(iter->status().IsCorruption());
  }
}

TEST(Block, PrefixCompressionShrinksSharedKeys) {
  BlockBuilder compressed(16), uncompressed(1);
  for (int i = 0; i < 100; i++) {
    const std::string key = "a-long-shared-prefix-" + std::to_string(1000 + i);
    compressed.Add(key, "v");
    uncompressed.Add(key, "v");
  }
  ASSERT_LT(compressed.CurrentSizeEstimate() * 2, uncompressed.CurrentSizeEstimate());
  ASSERT_EQ(compressed.CurrentSizeEstimate(), compressed.Finish().size());
}

TEST(Block, RestartArrayLayout) {
  // Three entries, restart interval 2: restart points at entries 0 and 2.
  BlockBuilder builder(2);
  builder.Add("apple", "1");
  builder.Add("apply", "2");
  builder.Add("banana", "3");
  const std::string block(builder.Finish());

  const std::string entries = std::string("\x00\x05\x01", 3) + "apple1" +  // full key
                              std::string("\x04\x01\x01", 3) + "y2" +      // shares "appl"
                              std::string("\x00\x06\x01", 3) + "banana3";  // restart: full key
  std::string expected = entries;
  PutFixed32(&expected, 0);
  PutFixed32(&expected, 9 + 5);  // offset of the third entry
  PutFixed32(&expected, 2);
  ASSERT_EQ(expected, block);
}

// --- Tables ---

class TableTest : public testing::Test {
 protected:
  TableTest() {
    options_.comparator = &kInternalComparator;
    options_.block_size = 256;
    options_.block_restart_interval = 4;
  }

  // What a point lookup through InternalGet found.
  struct GetResult {
    Status status;
    bool called = false;
    std::string key, value;
  };

  static GetResult Get(const Table& table, const std::string& internal_key,
                       const ReadOptions& read_options = ReadOptions()) {
    GetResult result;
    result.status = table.InternalGet(read_options, internal_key, &result,
                                      [](void* arg, Slice key, Slice value) {
                                        auto* r = static_cast<GetResult*>(arg);
                                        r->called = true;
                                        r->key.assign(key);
                                        r->value.assign(value);
                                      });
    return result;
  }

  // 200 user keys, one version each.
  static Model SequentialModel() {
    Model model;
    for (int i = 0; i < 200; i++) {
      model[IKey("key" + std::to_string(1000 + i), 5)] = "value" + std::to_string(i);
    }
    return model;
  }

  TableOptions options_;
};

TEST_F(TableTest, GetFindsEveryKey) {
  const Model model = SequentialModel();
  std::unique_ptr<Table> table;
  ASSERT_OK(OpenTableFile(options_, BuildTableFile(options_, model), &table));

  for (const auto& [key, value] : model) {
    // A lookup carries a sequence at or above the stored one.
    const std::string lookup = IKey(ExtractUserKey(key), 9);
    const GetResult r = Get(*table, lookup);
    ASSERT_OK(r.status);
    ASSERT_TRUE(r.called);
    ASSERT_EQ(key, r.key);
    ASSERT_EQ(value, r.value);
  }
}

TEST_F(TableTest, BloomFilterScreensOutAbsentKeys) {
  const Model model = SequentialModel();
  std::unique_ptr<Table> table;
  ASSERT_OK(OpenTableFile(options_, BuildTableFile(options_, model), &table));

  int passed_filter = 0;
  for (int i = 0; i < 1000; i++) {
    const GetResult r = Get(*table, IKey("absent" + std::to_string(i), 9));
    ASSERT_OK(r.status);
    // The filter can pass an absent key, but then the entry found must be
    // for some other user key.
    if (r.called) {
      passed_filter++;
      ASSERT_NE("absent" + std::to_string(i), ExtractUserKey(r.key));
    }
  }
  ASSERT_LT(passed_filter, 50);  // about 1% expected at 10 bits per key
}

TEST_F(TableTest, WithoutAFilterAbsentKeysReachTheBlocks) {
  options_.bloom_bits_per_key = 0;
  const Model model = SequentialModel();
  std::unique_ptr<Table> table;
  ASSERT_OK(OpenTableFile(options_, BuildTableFile(options_, model), &table));

  // "key1050x" sorts between two stored keys, so the lookup lands on the
  // one after it.
  const GetResult r = Get(*table, IKey("key1050x", 9));
  ASSERT_OK(r.status);
  ASSERT_TRUE(r.called);
  ASSERT_EQ("key1051", ExtractUserKey(r.key));

  // Past the last key there is nothing to land on.
  ASSERT_FALSE(Get(*table, IKey("zzz", 9)).called);
}

TEST_F(TableTest, IndexSeparatorsAreShortened) {
  // One entry per block, so the index has an entry per key. If it stored
  // full keys, lengthening every key would grow the table twice over: once
  // in the data blocks and once in the index. It stores the shortest key
  // that separates two blocks instead, here a single character, so only
  // the data blocks grow.
  options_.block_size = 1;
  options_.bloom_bits_per_key = 0;
  constexpr int kKeys = 13;
  constexpr int kExtra = 60;
  Model long_keys, short_keys;
  for (int i = 0; i < kKeys; i++) {
    const std::string first(1, static_cast<char>('a' + 2 * i));  // a, c, e, ...
    long_keys[IKey(first + std::string(kExtra, 'x'), 1)] = "v";
    short_keys[IKey(first, 1)] = "v";
  }
  const size_t long_size = BuildTableFile(options_, long_keys).size();
  const size_t short_size = BuildTableFile(options_, short_keys).size();
  ASSERT_GE(long_size, short_size + kKeys * kExtra);
  // Slack for block handles, whose varints grow with the larger offsets.
  ASSERT_LT(long_size, short_size + kKeys * kExtra + kKeys * 4);
}

TEST_F(TableTest, BlockCacheServesRepeatedReads) {
  ShardedLRUCache cache(1 << 20);
  options_.block_cache = &cache;
  const Model model = SequentialModel();
  std::unique_ptr<Table> table;
  ASSERT_OK(OpenTableFile(options_, BuildTableFile(options_, model), &table));

  auto scan = [&](const ReadOptions& read_options) {
    std::unique_ptr<Iterator> iter(table->NewIterator(read_options));
    size_t n = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) n++;
    EXPECT_OK(iter->status());
    EXPECT_EQ(model.size(), n);
  };

  ReadOptions no_fill;
  no_fill.fill_cache = false;
  scan(no_fill);
  ASSERT_EQ(0u, cache.TotalCharge());  // read, but not cached
  const uint64_t blocks = cache.misses();
  ASSERT_GT(blocks, 5u);

  scan(ReadOptions());
  ASSERT_GT(cache.TotalCharge(), 0u);
  ASSERT_EQ(0u, cache.hits());

  scan(ReadOptions());
  ASSERT_EQ(blocks, cache.hits());       // every block came from the cache
  ASSERT_EQ(2 * blocks, cache.misses());  // and none was read again
}

TEST_F(TableTest, OpenRejectsFilesThatAreNotTables) {
  std::unique_ptr<Table> table;
  ASSERT_TRUE(OpenTableFile(options_, "", &table).IsCorruption());
  ASSERT_TRUE(OpenTableFile(options_, std::string(47, 'x'), &table).IsCorruption());
  ASSERT_TRUE(OpenTableFile(options_, std::string(4096, '\0'), &table).IsCorruption());
  ASSERT_EQ(nullptr, table);

  // A valid table cut short anywhere is rejected too.
  const std::string file = BuildTableFile(options_, SequentialModel());
  for (size_t len = 0; len < file.size(); len += 7) {
    ASSERT_FALSE(OpenTableFile(options_, file.substr(0, len), &table).ok()) << len;
  }
}

// Flip every bit of a table file, one at a time. Whatever is then read
// from it must be either exactly the original contents or an error:
// never different data reported as success.
TEST_F(TableTest, EverySingleBitFlipIsDetectedOrHarmless) {
  Model model;
  for (int i = 0; i < 40; i++) {
    model[IKey("key" + std::to_string(100 + i), 5)] = "value" + std::to_string(i);
  }
  options_.block_size = 128;
  const std::string original = BuildTableFile(options_, model);

  size_t detected = 0, harmless = 0;
  for (size_t byte = 0; byte < original.size(); byte++) {
    for (int bit = 0; bit < 8; bit++) {
      std::string damaged = original;
      damaged[byte] ^= static_cast<char>(1 << bit);

      std::unique_ptr<Table> table;
      Status s = OpenTableFile(options_, damaged, &table);
      Model scanned;
      if (s.ok()) {
        std::unique_ptr<Iterator> iter(table->NewIterator(ReadOptions()));
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
          scanned.emplace(iter->key(), iter->value());
        }
        s = iter->status();
      }
      // Point lookups take a different path (filter, then one block).
      bool lookups_correct = true;
      if (s.ok()) {
        for (const auto& [key, value] : model) {
          const GetResult r = Get(*table, key);
          if (!r.status.ok()) {
            s = r.status;
            break;
          }
          if (!r.called || r.key != key || r.value != value) lookups_correct = false;
        }
      }

      if (!s.ok()) {
        ASSERT_TRUE(s.IsCorruption()) << s.ToString();
        detected++;
        continue;
      }
      ASSERT_EQ(model, scanned) << "byte=" << byte << " bit=" << bit << ": scan returned wrong data";
      ASSERT_TRUE(lookups_correct) << "byte=" << byte << " bit=" << bit << ": Get returned wrong data";
      // The only bytes no checksum covers are the footer's handles and
      // padding, and a flip there is harmless only in the padding.
      ASSERT_GE(byte, original.size() - Footer::kEncodedLength) << "bit=" << bit;
      ASSERT_LT(byte, original.size() - 8) << "bit=" << bit;
      harmless++;
    }
  }
  ASSERT_GT(detected, (original.size() - Footer::kEncodedLength) * 8 - 1);
  std::printf("  %zu bit flips: %zu detected, %zu harmless (footer padding)\n",
              original.size() * 8, detected, harmless);
}

}  // namespace lsm
