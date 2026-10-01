#pragma once

#include <atomic>
#include <cassert>
#include <new>
#include <random>

#include "util/arena.h"

namespace lsm {

// Ordered set backed by an arena.
//
// Thread safety: writes require external synchronization (one writer at a
// time). Reads require no locking and may run concurrently with the writer,
// provided the SkipList is not destroyed while a read is in progress.
//
// Invariants that make lock-free reads safe:
//   1. Nodes are never deleted until the SkipList (its arena) is destroyed.
//   2. A node's contents other than its next pointers are immutable once it
//      is linked in. Insert initializes a node fully, then publishes it with
//      release stores bottom-up, so a reader that reaches a node through an
//      acquire load sees an initialized node. A reader may see a tower
//      linked at only its lower levels; level 0 is always complete.
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // cmp(a, b) returns <0, 0, >0. Nodes are allocated from arena, which must
  // outlive the list.
  SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: nothing that compares equal to key is in the list.
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Prev() {
      // No back pointers: search for the last node before the current key.
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) node_ = nullptr;
    }
    // Advances to the first entry >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    void SeekToLast() {
      node_ = list_->FindLast();
      if (node_ == list_->head_) node_ = nullptr;
    }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr int kBranching = 4;

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && compare_(n->key, key) < 0;
  }

  // Returns the earliest node >= key, or nullptr. If prev is non-null, fills
  // prev[level] with the last node < key at every level.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
  // Returns the latest node < key, or head_ if there is none.
  Node* FindLessThan(const Key& key) const;
  // Returns the last node, or head_ if the list is empty.
  Node* FindLast() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;

  // Written only by Insert; read racily by readers. A stale value is fine:
  // see the comment in Insert.
  std::atomic<int> max_height_{1};

  std::minstd_rand rnd_{0xdeadbeef};  // used only by Insert
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  Node* Next(int n) {
    assert(n >= 0);
    // Acquire, so that the node it returns is seen fully initialized.
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // Release, so that anyone reading this pointer sees x initialized.
    next_[n].store(x, std::memory_order_release);
  }

  // For the few places where ordering is provided some other way.
  Node* NoBarrier_Next(int n) { return next_[n].load(std::memory_order_relaxed); }
  void NoBarrier_SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

 private:
  // Really of length equal to the node's height; next_[0] is the lowest level.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int height) {
  char* const mem =
      arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (mem) Node(key);
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)) {
  for (int i = 0; i < kMaxHeight; i++) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // Each level is kept with probability 1/kBranching.
  int height = 1;
  while (height < kMaxHeight && rnd_() % kBranching == 0) {
    height++;
  }
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep searching in this level
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      level--;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLessThan(
    const Key& key) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    assert(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    if (next == nullptr || compare_(next->key, key) >= 0) {
      if (level == 0) return x;
      level--;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) return x;
      level--;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  assert(x == nullptr || compare_(key, x->key) != 0);  // no duplicates
  (void)x;

  const int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // No synchronization with readers is needed. A reader that sees the new
    // max_height_ either sees nullptr in head_'s new levels and drops down a
    // level, or sees the new node, which is fully linked below by then.
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // x is not yet reachable, so its own pointers need no barrier. Linking
    // prev[i] to it publishes it and must be a release store.
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && compare_(key, x->key) == 0;
}

}  // namespace lsm
