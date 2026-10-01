#include "table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "util/coding.h"

namespace lsm {

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
  assert(restart_interval >= 1);
  restarts_.push_back(0);  // the first restart point is at offset 0
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

Slice BlockBuilder::Finish() {
  for (uint32_t restart : restarts_) {
    PutFixed32(&buffer_, restart);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

void BlockBuilder::Add(Slice key, Slice value) {
  assert(!finished_);
  assert(counter_ <= restart_interval_);

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    // Share as much of the previous key as possible.
    const size_t min_length = std::min(last_key_.size(), key.size());
    while (shared < min_length && last_key_[shared] == key[shared]) {
      shared++;
    }
  } else {
    // Restart: store the whole key.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value);

  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(last_key_ == key);
  counter_++;
}

}  // namespace lsm
