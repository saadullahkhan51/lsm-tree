#include "table/merger.h"

#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

namespace {

using Model = std::map<std::string, std::string>;

// An iterator over a sorted map, which can be told to fail once it has
// yielded a given number of entries.
class MapIterator : public Iterator {
 public:
  explicit MapIterator(Model data, int fail_after = -1)
      : data_(std::move(data)), it_(data_.end()), fail_after_(fail_after) {}

  bool Valid() const override { return !failed_ && it_ != data_.end(); }
  void SeekToFirst() override { Move([&] { it_ = data_.begin(); }); }
  void SeekToLast() override {
    Move([&] { it_ = data_.empty() ? data_.end() : std::prev(data_.end()); });
  }
  void Seek(Slice target) override { Move([&] { it_ = data_.lower_bound(std::string(target)); }); }
  void Next() override { Move([&] { ++it_; }); }
  void Prev() override {
    Move([&] { it_ = it_ == data_.begin() ? data_.end() : std::prev(it_); });
  }
  Slice key() const override { return it_->first; }
  Slice value() const override { return it_->second; }
  Status status() const override {
    return failed_ ? Status::Corruption("injected failure") : Status::OK();
  }

 private:
  template <typename F>
  void Move(F f) {
    if (failed_) return;
    f();
    if (fail_after_ >= 0 && moves_++ >= fail_after_) failed_ = true;
  }

  const Model data_;
  Model::const_iterator it_;
  const int fail_after_;
  int moves_ = 0;
  bool failed_ = false;
};

std::string ToString(const Iterator* iter) {
  return iter->Valid() ? std::string(iter->key()) + "->" + std::string(iter->value()) : "END";
}

std::string ToString(const Model& model, Model::const_iterator it) {
  return it == model.end() ? "END" : it->first + "->" + it->second;
}

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%05d", i);
  return buf;
}

// Splits keys [0, n) among `children` iterators at random and returns the
// merge of them, with the union in *model.
Iterator* RandomMerge(std::mt19937* rnd, int n, int children, Model* model) {
  std::vector<Model> parts(children);
  for (int i = 0; i < n; i++) {
    const int child = (*rnd)() % children;
    parts[child][Key(i)] = "v" + std::to_string(i) + "-from-" + std::to_string(child);
    (*model)[Key(i)] = parts[child][Key(i)];
  }
  std::vector<Iterator*> iters;
  for (Model& part : parts) iters.push_back(new MapIterator(std::move(part)));
  return NewMergingIterator(BytewiseComparator(), std::move(iters));
}

}  // namespace

TEST(MergingIterator, NoChildren) {
  std::unique_ptr<Iterator> iter(NewMergingIterator(BytewiseComparator(), {}));
  iter->SeekToFirst();
  ASSERT_FALSE(iter->Valid());
  ASSERT_OK(iter->status());
}

TEST(MergingIterator, ForwardAndBackwardScans) {
  for (int children : {1, 2, 3, 8, 30}) {
    std::mt19937 rnd(children);
    Model model;
    std::unique_ptr<Iterator> iter(RandomMerge(&rnd, 500, children, &model));

    iter->SeekToFirst();
    for (auto it = model.begin(); it != model.end(); ++it, iter->Next()) {
      ASSERT_EQ(ToString(model, it), ToString(iter.get()));
    }
    ASSERT_FALSE(iter->Valid());

    iter->SeekToLast();
    for (auto it = model.rbegin(); it != model.rend(); ++it, iter->Prev()) {
      ASSERT_EQ(it->first + "->" + it->second, ToString(iter.get()));
    }
    ASSERT_FALSE(iter->Valid());
    ASSERT_OK(iter->status());
  }
}

TEST(MergingIterator, EmptyChildrenAreSkipped) {
  std::vector<Iterator*> children = {
      new MapIterator({}),
      new MapIterator({{"b", "2"}}),
      new MapIterator({}),
      new MapIterator({{"a", "1"}, {"c", "3"}}),
      new MapIterator({}),
  };
  std::unique_ptr<Iterator> iter(NewMergingIterator(BytewiseComparator(), std::move(children)));
  std::string seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) seen += std::string(iter->key());
  ASSERT_EQ("abc", seen);
  seen.clear();
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) seen += std::string(iter->key());
  ASSERT_EQ("cba", seen);
}

// Mixes seeks and steps in both directions. Changing direction is the hard
// part: the children that are not current have to be repositioned on the
// other side of the current key.
TEST(MergingIterator, RandomWalkWithDirectionChanges) {
  for (int children : {2, 3, 7}) {
    for (uint32_t seed = 0; seed < 20; seed++) {
      SCOPED_TRACE("children=" + std::to_string(children) + " seed=" + std::to_string(seed));
      std::mt19937 rnd(seed * 100 + children);
      Model model;
      const int n = 1 + rnd() % 200;
      std::unique_ptr<Iterator> iter(RandomMerge(&rnd, n, children, &model));

      auto model_it = model.end();
      for (int step = 0; step < 500; step++) {
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
            // Between keys as often as on one.
            const std::string target = Key(rnd() % (n + 2)) + (rnd() % 2 ? "" : "x");
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

// A child that fails must stop the merge. Carrying on with the other
// children would return a result with that child's entries missing.
TEST(MergingIterator, AChildErrorStopsTheMerge) {
  Model failing, healthy;
  for (int i = 0; i < 100; i += 2) failing[Key(i)] = "f";
  for (int i = 1; i < 100; i += 2) healthy[Key(i)] = "h";

  std::vector<Iterator*> children = {new MapIterator(healthy),
                                     new MapIterator(failing, /*fail_after=*/10)};
  std::unique_ptr<Iterator> iter(NewMergingIterator(BytewiseComparator(), std::move(children)));

  int yielded = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) yielded++;
  ASSERT_TRUE(iter->status().IsCorruption());
  ASSERT_LT(yielded, 30);  // stopped near the failure, not at the end of the healthy child

  // And it stays stopped.
  iter->SeekToFirst();
  ASSERT_FALSE(iter->Valid());
  ASSERT_TRUE(iter->status().IsCorruption());
}

}  // namespace lsm
