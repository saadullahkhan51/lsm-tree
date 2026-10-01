#include "table/merger.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>

namespace lsm {

namespace {

// Keeps the valid children in a binary heap ordered by their current key:
// a min-heap while moving forward, a max-heap while moving backward. The
// top of the heap is the current entry.
class MergingIterator : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, std::vector<Iterator*> children)
      : comparator_(comparator) {
    children_.reserve(children.size());
    for (Iterator* child : children) children_.emplace_back(child);
    heap_.reserve(children_.size());
  }

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    direction_ = kForward;
    RebuildHeap();
  }

  void SeekToLast() override {
    for (auto& child : children_) child->SeekToLast();
    direction_ = kReverse;
    RebuildHeap();
  }

  void Seek(Slice target) override {
    for (auto& child : children_) child->Seek(target);
    direction_ = kForward;
    RebuildHeap();
  }

  void Next() override {
    assert(Valid());
    if (direction_ != kForward) {
      // The other children are positioned before key(). Move each to its
      // first entry after key(), so that all of them are ahead of current_.
      const std::string current_key(key());
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->Seek(current_key);
        if (child->Valid() && comparator_->Compare(current_key, child->key()) == 0) {
          child->Next();
        }
      }
      direction_ = kForward;
      current_->Next();
      RebuildHeap();
      return;
    }
    AdvanceCurrent([](Iterator* it) { it->Next(); });
  }

  void Prev() override {
    assert(Valid());
    if (direction_ != kReverse) {
      // The other children are positioned at or after key(). Move each to
      // its last entry before key().
      const std::string current_key(key());
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->Seek(current_key);
        if (child->Valid()) {
          child->Prev();  // the entry before the first one >= key()
        } else {
          child->SeekToLast();  // everything in this child is < key()
        }
      }
      direction_ = kReverse;
      current_->Prev();
      RebuildHeap();
      return;
    }
    AdvanceCurrent([](Iterator* it) { it->Prev(); });
  }

  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  Status status() const override { return status_; }

 private:
  enum Direction { kForward, kReverse };

  // Heap ordering. std::*_heap keeps the element that compares greatest at
  // the front, so the comparison is inverted for the forward min-heap.
  bool HeapLess(const Iterator* a, const Iterator* b) const {
    const int c = comparator_->Compare(a->key(), b->key());
    return direction_ == kForward ? c > 0 : c < 0;
  }

  // Moves the current child one step and restores the heap.
  template <typename Step>
  void AdvanceCurrent(Step step) {
    auto less = [this](const Iterator* a, const Iterator* b) { return HeapLess(a, b); };
    step(current_);
    // The top's key changed. Take it out and, if it still has an entry, put
    // it back where it now belongs.
    std::pop_heap(heap_.begin(), heap_.end(), less);
    if (current_->Valid()) {
      std::push_heap(heap_.begin(), heap_.end(), less);
    } else {
      heap_.pop_back();
      if (!current_->status().ok()) {
        Fail(current_->status());
        return;
      }
    }
    current_ = heap_.empty() ? nullptr : heap_.front();
  }

  void RebuildHeap() {
    heap_.clear();
    current_ = nullptr;
    if (!status_.ok()) return;  // an earlier failure is final
    for (auto& child : children_) {
      if (child->Valid()) {
        heap_.push_back(child.get());
      } else if (!child->status().ok()) {
        Fail(child->status());
        return;
      }
    }
    std::make_heap(heap_.begin(), heap_.end(),
                   [this](const Iterator* a, const Iterator* b) { return HeapLess(a, b); });
    current_ = heap_.empty() ? nullptr : heap_.front();
  }

  void Fail(const Status& s) {
    if (status_.ok()) status_ = s;
    heap_.clear();
    current_ = nullptr;
  }

  const Comparator* const comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;  // children that are Valid()
  Iterator* current_ = nullptr;  // == heap_.front(), or null if !Valid()
  Direction direction_ = kForward;
  Status status_;
};

}  // namespace

Iterator* NewMergingIterator(const Comparator* comparator, std::vector<Iterator*> children) {
  if (children.empty()) return NewEmptyIterator();
  if (children.size() == 1) return children[0];
  return new MergingIterator(comparator, std::move(children));
}

}  // namespace lsm
