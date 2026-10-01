#pragma once

#include <cassert>

#include "db/dbformat.h"
#include "lsm/db.h"

namespace lsm {

class SnapshotList;

// A pinned sequence number. Snapshots live in a doubly-linked list on the
// database, oldest first, so that the oldest one, which bounds what
// compaction may discard, is found in constant time.
class SnapshotImpl : public Snapshot {
 public:
  explicit SnapshotImpl(SequenceNumber sequence) : sequence_(sequence) {}

  SequenceNumber sequence() const { return sequence_; }

 private:
  friend class SnapshotList;

  const SequenceNumber sequence_;
  SnapshotImpl* prev_ = nullptr;
  SnapshotImpl* next_ = nullptr;
};

// All methods require the database mutex.
class SnapshotList {
 public:
  SnapshotList() : head_(0) {
    head_.prev_ = &head_;
    head_.next_ = &head_;
  }

  bool empty() const { return head_.next_ == &head_; }
  SnapshotImpl* oldest() const {
    assert(!empty());
    return head_.next_;
  }
  SnapshotImpl* newest() const {
    assert(!empty());
    return head_.prev_;
  }

  // Appends a snapshot. Sequence numbers never decrease, so the list stays
  // ordered.
  SnapshotImpl* New(SequenceNumber sequence) {
    assert(empty() || newest()->sequence_ <= sequence);
    auto* snapshot = new SnapshotImpl(sequence);
    snapshot->next_ = &head_;
    snapshot->prev_ = head_.prev_;
    snapshot->prev_->next_ = snapshot;
    snapshot->next_->prev_ = snapshot;
    return snapshot;
  }

  void Delete(const SnapshotImpl* snapshot) {
    snapshot->prev_->next_ = snapshot->next_;
    snapshot->next_->prev_ = snapshot->prev_;
    delete snapshot;
  }

  // Calls f(sequence) for each snapshot, oldest first.
  template <typename F>
  void ForEach(F f) const {
    for (const SnapshotImpl* s = head_.next_; s != &head_; s = s->next_) f(s->sequence_);
  }

 private:
  SnapshotImpl head_;  // dummy node of the circular list
};

}  // namespace lsm
