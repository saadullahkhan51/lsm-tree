#include "table/block.h"

#include <algorithm>
#include <cassert>
#include <string>

#include "util/coding.h"

namespace lsm {

uint32_t Block::NumRestarts() const {
  assert(size_ >= sizeof(uint32_t));
  return DecodeFixed32(data_.get() + size_ - sizeof(uint32_t));
}

Block::Block(BlockContents contents) : data_(std::move(contents.data)), size_(contents.size) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;
  } else {
    const size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    if (NumRestarts() > max_restarts_allowed) {
      size_ = 0;  // the restart array cannot fit in the block
    } else {
      restart_offset_ = static_cast<uint32_t>(size_ - (1 + NumRestarts()) * sizeof(uint32_t));
    }
  }
}

namespace {

// Decodes the entry header at p. Returns the position of the key delta, or
// nullptr if the entry does not fit before limit.
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared, uint32_t* non_shared,
                        uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = static_cast<uint8_t>(p[0]);
  *non_shared = static_cast<uint8_t>(p[1]);
  *value_length = static_cast<uint8_t>(p[2]);
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three lengths are one byte each.
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }
  if (static_cast<uint64_t>(limit - p) < uint64_t{*non_shared} + *value_length) {
    return nullptr;
  }
  return p;
}

}  // namespace

class Block::Iter : public Iterator {
 public:
  Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts),
        restart_index_(num_restarts),
        value_(data + restarts, 0) {
    assert(num_restarts_ > 0);
  }

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  Slice key() const override {
    assert(Valid());
    return key_;
  }
  Slice value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void Prev() override {
    assert(Valid());

    // Back up to the restart point before the current entry, then scan
    // forward to the entry just before it.
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        // No entry precedes the first one.
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    do {
      // Loop until the end of the current entry reaches the original one.
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  void Seek(Slice target) override {
    // Binary search the restart array for the last restart point whose key
    // is < target.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                                        &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      const Slice mid_key(key_ptr, non_shared);
      if (comparator_->Compare(mid_key, target) < 0) {
        left = mid;  // key at mid is smaller than target: everything before mid is too
      } else {
        right = mid - 1;  // key at mid is >= target: everything at or after mid is too
      }
    }

    // Linear scan within the restart region for the first key >= target.
    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) return;
      if (comparator_->Compare(key_, target) >= 0) return;
    }
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
      // Keep skipping to the last entry.
    }
  }

 private:
  // Offset just past the current entry.
  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  uint32_t GetRestartPoint(uint32_t index) const {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    // current_ is fixed up by ParseNextKey, which starts at the end of
    // value_; point value_ at the restart offset with zero length.
    // A restart offset past the entries can only come from a damaged block;
    // clamping it makes the next parse see the end of the block.
    const uint32_t offset = std::min(GetRestartPoint(index), restarts_);
    value_ = Slice(data_ + offset, 0);
  }

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = Slice(data_ + restarts_, 0);
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;  // entries end where the restart array begins
    if (p >= limit) {
      // No more entries. Mark as invalid.
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = Slice(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  const Comparator* const comparator_;
  const char* const data_;      // the block's bytes
  const uint32_t restarts_;     // offset of the restart array
  const uint32_t num_restarts_;

  uint32_t current_;        // offset of the current entry; >= restarts_ if !Valid
  uint32_t restart_index_;  // restart region current_ falls in
  std::string key_;
  Slice value_;
  Status status_;
};

Iterator* Block::NewIterator(const Comparator* comparator) const {
  if (size_ < sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return NewEmptyIterator();
  }
  return new Iter(comparator, data_.get(), restart_offset_, num_restarts);
}

}  // namespace lsm
