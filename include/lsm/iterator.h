#pragma once

#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// Walks a sorted sequence of key/value pairs. Not thread-safe: one thread
// per iterator, though separate iterators over the same source are fine.
class Iterator {
 public:
  Iterator();
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator();

  // True if positioned at an entry. Every other accessor requires Valid().
  virtual bool Valid() const = 0;

  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;
  // Positions at the first entry with key >= target.
  virtual void Seek(Slice target) = 0;
  virtual void Next() = 0;
  virtual void Prev() = 0;

  // The returned slices stay valid only until the iterator is moved.
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;

  // An iterator that hits an error becomes !Valid() and reports it here.
  // Check it after any loop that ends on !Valid().
  virtual Status status() const = 0;

  // Runs function(arg1, arg2) when the iterator is destroyed. Used to
  // release whatever the iterator pins (a cache handle, a version).
  using CleanupFunction = void (*)(void* arg1, void* arg2);
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  // The first node is stored inline so that the common case of one cleanup
  // needs no allocation.
  struct CleanupNode {
    CleanupFunction function = nullptr;
    void* arg1;
    void* arg2;
    CleanupNode* next = nullptr;
  };
  CleanupNode cleanup_head_;
};

// An iterator over nothing, optionally carrying an error.
Iterator* NewEmptyIterator();
Iterator* NewErrorIterator(const Status& status);

}  // namespace lsm
