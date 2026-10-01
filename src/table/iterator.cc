#include "lsm/iterator.h"

#include <cassert>

namespace lsm {

Iterator::Iterator() = default;

Iterator::~Iterator() {
  if (cleanup_head_.function == nullptr) return;
  cleanup_head_.function(cleanup_head_.arg1, cleanup_head_.arg2);
  for (CleanupNode* node = cleanup_head_.next; node != nullptr;) {
    node->function(node->arg1, node->arg2);
    CleanupNode* next = node->next;
    delete node;
    node = next;
  }
}

void Iterator::RegisterCleanup(CleanupFunction function, void* arg1, void* arg2) {
  assert(function != nullptr);
  CleanupNode* node = &cleanup_head_;
  if (cleanup_head_.function != nullptr) {
    node = new CleanupNode();
    node->next = cleanup_head_.next;
    cleanup_head_.next = node;
  }
  node->function = function;
  node->arg1 = arg1;
  node->arg2 = arg2;
}

namespace {

class EmptyIterator : public Iterator {
 public:
  explicit EmptyIterator(const Status& s) : status_(s) {}

  bool Valid() const override { return false; }
  void Seek(Slice) override {}
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Next() override { assert(false); }
  void Prev() override { assert(false); }
  Slice key() const override {
    assert(false);
    return Slice();
  }
  Slice value() const override {
    assert(false);
    return Slice();
  }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

Iterator* NewEmptyIterator() { return new EmptyIterator(Status::OK()); }

Iterator* NewErrorIterator(const Status& status) { return new EmptyIterator(status); }

}  // namespace lsm
