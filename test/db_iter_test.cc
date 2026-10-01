#include "db/db_iter.h"

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "db/memtable.h"
#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

namespace {

using Model = std::map<std::string, std::string>;

struct Entry {
  std::string key;
  SequenceNumber sequence;
  ValueType type;
  std::string value;
};

// A memtable holding the given internal entries, and DB iterators over it.
class Source {
 public:
  explicit Source(const std::vector<Entry>& entries)
      : entries_(entries), mem_(new MemTable(InternalKeyComparator(BytewiseComparator()))) {
    mem_->Ref();
    for (const Entry& e : entries_) mem_->Add(e.sequence, e.type, e.key, e.value);
  }
  ~Source() { mem_->Unref(); }

  std::unique_ptr<Iterator> NewIterator(SequenceNumber sequence) const {
    return std::unique_ptr<Iterator>(
        NewDBIterator(BytewiseComparator(), mem_->NewIterator(), sequence));
  }

  // What a reader at `sequence` should see, computed directly.
  Model View(SequenceNumber sequence) const {
    std::map<std::string, const Entry*> newest;
    for (const Entry& e : entries_) {
      if (e.sequence > sequence) continue;
      const Entry*& slot = newest[e.key];
      if (slot == nullptr || slot->sequence < e.sequence) slot = &e;
    }
    Model view;
    for (const auto& [key, e] : newest) {
      if (e->type == kTypeValue) view[key] = e->value;
    }
    return view;
  }

 private:
  const std::vector<Entry> entries_;
  MemTable* mem_;
};

Entry Put(const std::string& key, SequenceNumber seq, const std::string& value) {
  return {key, seq, kTypeValue, value};
}
Entry Del(const std::string& key, SequenceNumber seq) { return {key, seq, kTypeDeletion, ""}; }

std::string ToString(const Iterator* iter) {
  return iter->Valid() ? std::string(iter->key()) + "->" + std::string(iter->value()) : "END";
}
std::string ToString(const Model& model, Model::const_iterator it) {
  return it == model.end() ? "END" : it->first + "->" + it->second;
}

// The forward and the backward scan, as "k->v " strings.
std::string Forward(Iterator* iter) {
  std::string out;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) out += ToString(iter) + " ";
  return out;
}
std::string Backward(Iterator* iter) {
  std::string out;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) out += ToString(iter) + " ";
  return out;
}

}  // namespace

TEST(DBIter, Empty) {
  const Source source({});
  auto iter = source.NewIterator(100);
  ASSERT_FALSE(iter->Valid());
  ASSERT_EQ("", Forward(iter.get()));
  ASSERT_EQ("", Backward(iter.get()));
  iter->Seek("a");
  ASSERT_FALSE(iter->Valid());
  ASSERT_OK(iter->status());
}

TEST(DBIter, YieldsTheNewestVersionOfEachKey) {
  const Source source({Put("a", 1, "a1"), Put("a", 5, "a5"), Put("a", 9, "a9"), Put("b", 2, "b2"),
                       Put("c", 3, "c3"), Put("c", 4, "c4")});
  auto iter = source.NewIterator(100);
  ASSERT_EQ("a->a9 b->b2 c->c4 ", Forward(iter.get()));
  ASSERT_EQ("c->c4 b->b2 a->a9 ", Backward(iter.get()));
}

TEST(DBIter, ReadsAsOfItsSequence) {
  const Source source({Put("a", 1, "a1"), Put("a", 5, "a5"), Put("b", 3, "b3"), Del("b", 6),
                       Put("c", 7, "c7")});
  ASSERT_EQ("", Forward(source.NewIterator(0).get()));
  ASSERT_EQ("a->a1 ", Forward(source.NewIterator(2).get()));
  ASSERT_EQ("a->a1 b->b3 ", Forward(source.NewIterator(4).get()));
  ASSERT_EQ("a->a5 b->b3 ", Forward(source.NewIterator(5).get()));
  ASSERT_EQ("a->a5 ", Forward(source.NewIterator(6).get()));  // b deleted at 6
  ASSERT_EQ("a->a5 c->c7 ", Forward(source.NewIterator(7).get()));

  ASSERT_EQ("b->b3 a->a1 ", Backward(source.NewIterator(4).get()));
  ASSERT_EQ("a->a5 ", Backward(source.NewIterator(6).get()));
  ASSERT_EQ("c->c7 a->a5 ", Backward(source.NewIterator(7).get()));
}

TEST(DBIter, DeletedKeysAreSkippedInBothDirections) {
  const Source source({Put("a", 1, "a1"), Put("b", 2, "b2"), Del("b", 5), Put("c", 3, "c3"),
                       Del("c", 6), Put("d", 4, "d4"), Del("e", 7), Put("f", 8, "f8")});
  auto iter = source.NewIterator(100);
  ASSERT_EQ("a->a1 d->d4 f->f8 ", Forward(iter.get()));
  ASSERT_EQ("f->f8 d->d4 a->a1 ", Backward(iter.get()));
}

TEST(DBIter, DeletedThenRewrittenKeyIsLive) {
  const Source source({Put("a", 1, "old"), Del("a", 2), Put("a", 3, "new")});
  ASSERT_EQ("a->new ", Forward(source.NewIterator(100).get()));
  ASSERT_EQ("a->new ", Backward(source.NewIterator(100).get()));
  ASSERT_EQ("", Forward(source.NewIterator(2).get()));
  ASSERT_EQ("", Backward(source.NewIterator(2).get()));
  ASSERT_EQ("a->old ", Backward(source.NewIterator(1).get()));
}

TEST(DBIter, AllKeysDeleted) {
  const Source source({Put("a", 1, "a"), Del("a", 2), Put("b", 3, "b"), Del("b", 4)});
  auto iter = source.NewIterator(100);
  ASSERT_EQ("", Forward(iter.get()));
  ASSERT_EQ("", Backward(iter.get()));
  iter->Seek("a");
  ASSERT_FALSE(iter->Valid());
}

TEST(DBIter, SeekLandsOnTheFirstLiveKeyAtOrAfterTheTarget) {
  const Source source({Put("b", 1, "b1"), Put("b", 4, "b4"), Put("d", 2, "d2"), Del("d", 5),
                       Put("f", 3, "f3")});
  auto iter = source.NewIterator(100);
  iter->Seek("a");
  ASSERT_EQ("b->b4", ToString(iter.get()));
  iter->Seek("b");
  ASSERT_EQ("b->b4", ToString(iter.get()));
  iter->Seek("c");
  ASSERT_EQ("f->f3", ToString(iter.get()));  // d is deleted
  iter->Seek("d");
  ASSERT_EQ("f->f3", ToString(iter.get()));
  iter->Seek("g");
  ASSERT_EQ("END", ToString(iter.get()));

  // At an older sequence d is still there.
  auto old = source.NewIterator(4);
  old->Seek("c");
  ASSERT_EQ("d->d2", ToString(old.get()));
}

TEST(DBIter, DirectionChangesInTheMiddleOfManyVersions) {
  // Several versions per key, so that turning around has to cross all the
  // versions of the current key and then all of its neighbor's.
  std::vector<Entry> entries;
  SequenceNumber seq = 1;
  for (const char* key : {"a", "b", "c", "d"}) {
    for (int version = 0; version < 5; version++) {
      entries.push_back(Put(key, seq++, std::string(key) + std::to_string(version)));
    }
  }
  const Source source(entries);
  auto iter = source.NewIterator(100);

  iter->SeekToFirst();
  ASSERT_EQ("a->a4", ToString(iter.get()));
  iter->Next();
  ASSERT_EQ("b->b4", ToString(iter.get()));
  iter->Next();
  ASSERT_EQ("c->c4", ToString(iter.get()));
  iter->Prev();
  ASSERT_EQ("b->b4", ToString(iter.get()));
  iter->Prev();
  ASSERT_EQ("a->a4", ToString(iter.get()));
  iter->Next();
  ASSERT_EQ("b->b4", ToString(iter.get()));
  iter->Next();
  iter->Next();
  ASSERT_EQ("d->d4", ToString(iter.get()));
  iter->Next();
  ASSERT_EQ("END", ToString(iter.get()));

  iter->SeekToLast();
  ASSERT_EQ("d->d4", ToString(iter.get()));
  iter->Prev();
  iter->Next();
  ASSERT_EQ("d->d4", ToString(iter.get()));
  iter->Seek("b");
  iter->Prev();
  ASSERT_EQ("a->a4", ToString(iter.get()));
  iter->Prev();
  ASSERT_EQ("END", ToString(iter.get()));
}

TEST(DBIter, PrevFromTheFirstKeyAndNextFromTheLastEndTheScan) {
  const Source source({Del("0", 9), Put("a", 1, "a1"), Put("b", 2, "b2"), Del("z", 8)});
  auto iter = source.NewIterator(100);
  iter->SeekToFirst();
  ASSERT_EQ("a->a1", ToString(iter.get()));
  iter->Prev();  // only a deleted key lies before
  ASSERT_FALSE(iter->Valid());

  iter->SeekToLast();
  ASSERT_EQ("b->b2", ToString(iter.get()));
  iter->Next();  // only a deleted key lies after
  ASSERT_FALSE(iter->Valid());
  ASSERT_OK(iter->status());
}

TEST(DBIter, LargeValuesSurviveReverseIteration) {
  const std::string big1(2 << 20, 'x'), big2(3 << 20, 'y');
  const Source source({Put("a", 1, big1), Put("b", 2, "small"), Put("c", 3, big2)});
  auto iter = source.NewIterator(100);
  iter->SeekToLast();
  ASSERT_EQ(big2, iter->value());
  iter->Prev();
  ASSERT_EQ("small", iter->value());
  iter->Prev();
  ASSERT_EQ(big1, iter->value());
}

// Random entries, random read sequences, and random walks, each checked
// against the view computed directly from the entries.
TEST(DBIter, RandomizedAgainstModel) {
  for (uint32_t seed = 0; seed < 40; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rnd(seed);
    const int key_space = 1 + rnd() % 30;
    const int num_entries = rnd() % 300;
    std::vector<Entry> entries;
    for (int i = 0; i < num_entries; i++) {
      const std::string key = "k" + std::to_string(rnd() % key_space);
      const SequenceNumber seq = i + 1;  // unique, as in a real database
      if (rnd() % 3 == 0) {
        entries.push_back(Del(key, seq));
      } else {
        entries.push_back(Put(key, seq, "v" + std::to_string(seq)));
      }
    }
    const Source source(entries);

    for (int round = 0; round < 6; round++) {
      const SequenceNumber sequence = round == 0 ? kMaxSequenceNumber : rnd() % (num_entries + 2);
      SCOPED_TRACE("sequence " + std::to_string(sequence));
      const Model model = source.View(sequence);
      auto iter = source.NewIterator(sequence);

      auto model_it = model.end();
      for (int step = 0; step < 400; step++) {
        switch (rnd() % 6) {
          case 0:
          case 1:
            if (iter->Valid()) {
              iter->Next();
              ++model_it;
            }
            break;
          case 2:
          case 3:
            if (iter->Valid()) {
              iter->Prev();
              model_it = model_it == model.begin() ? model.end() : std::prev(model_it);
            }
            break;
          case 4: {
            const std::string target = "k" + std::to_string(rnd() % (key_space + 1));
            iter->Seek(target);
            model_it = model.lower_bound(target);
            break;
          }
          case 5:
            if (rnd() % 2) {
              iter->SeekToFirst();
              model_it = model.begin();
            } else {
              iter->SeekToLast();
              model_it = model.empty() ? model.end() : std::prev(model.end());
            }
            break;
        }
        ASSERT_EQ(ToString(model, model_it), ToString(iter.get())) << "step " << step;
      }
      ASSERT_OK(iter->status());
    }
  }
}

}  // namespace lsm
