#include "table/two_level_iterator.h"

#include <cassert>
#include <memory>
#include <string>

namespace lsm {

namespace {

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, const ReadOptions& options)
      : block_function_(std::move(block_function)), options_(options), index_iter_(index_iter) {}

  void Seek(Slice target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->Seek(target);
    SkipEmptyDataBlocksForward();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToFirst();
    SkipEmptyDataBlocksForward();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToLast();
    SkipEmptyDataBlocksBackward();
  }

  void Next() override {
    assert(Valid());
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  void Prev() override {
    assert(Valid());
    data_iter_->Prev();
    SkipEmptyDataBlocksBackward();
  }

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }
  Slice key() const override {
    assert(Valid());
    return data_iter_->key();
  }
  Slice value() const override {
    assert(Valid());
    return data_iter_->value();
  }

  Status status() const override {
    if (!index_iter_->status().ok()) return index_iter_->status();
    if (data_iter_ != nullptr && !data_iter_->status().ok()) return data_iter_->status();
    return status_;
  }

 private:
  // Remembers the first error from a data iterator that is about to be
  // replaced, so it is not lost.
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }

  void SkipEmptyDataBlocksForward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      // A block that failed to read stops the scan rather than being
      // skipped: silently stepping over it would hide its keys.
      if (data_iter_ != nullptr && !data_iter_->status().ok()) return;
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_ != nullptr) data_iter_->SeekToFirst();
    }
  }

  void SkipEmptyDataBlocksBackward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      if (data_iter_ != nullptr && !data_iter_->status().ok()) return;
      index_iter_->Prev();
      InitDataBlock();
      if (data_iter_ != nullptr) data_iter_->SeekToLast();
    }
  }

  void SetDataIterator(Iterator* data_iter) {
    if (data_iter_ != nullptr) SaveError(data_iter_->status());
    data_iter_.reset(data_iter);
  }

  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    const Slice handle = index_iter_->value();
    if (data_iter_ != nullptr && handle == data_block_handle_) {
      // Already positioned on this block.
      return;
    }
    Iterator* iter = block_function_(options_, handle);
    data_block_handle_.assign(handle);
    SetDataIterator(iter);
  }

  const BlockFunction block_function_;
  const ReadOptions options_;
  Status status_;
  const std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;  // may be null
  // The index value that data_iter_ was created from.
  std::string data_block_handle_;
};

}  // namespace

Iterator* NewTwoLevelIterator(Iterator* index_iter, BlockFunction block_function,
                              const ReadOptions& options) {
  return new TwoLevelIterator(index_iter, std::move(block_function), options);
}

}  // namespace lsm
