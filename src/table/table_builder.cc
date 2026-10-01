#include "table/table_builder.h"

#include <cassert>
#include <string>

#include "db/dbformat.h"
#include "lsm/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

struct TableBuilder::Rep {
  Rep(const TableOptions& opt, WritableFile* f)
      : options(opt),
        file(f),
        data_block(opt.block_restart_interval),
        // Index entries are looked up by binary search only, so every one
        // is a restart point.
        index_block(1) {
    if (opt.bloom_bits_per_key > 0) {
      filter_block = std::make_unique<FilterBlockBuilder>(opt.bloom_bits_per_key);
    }
  }

  bool ok() const { return status.ok(); }

  // Writes contents followed by its trailer and records where it went.
  void WriteRawBlock(Slice contents, BlockHandle* handle) {
    *handle = BlockHandle(offset, contents.size());
    status = file->Append(contents);
    if (!ok()) return;

    char trailer[kBlockTrailerSize];
    trailer[0] = kNoCompression;
    uint32_t crc = crc32c::Value(contents.data(), contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // cover the type byte
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    status = file->Append(Slice(trailer, kBlockTrailerSize));
    if (ok()) offset += contents.size() + kBlockTrailerSize;
  }

  void WriteBlock(BlockBuilder* block, BlockHandle* handle) {
    WriteRawBlock(block->Finish(), handle);
    block->Reset();
  }

  const TableOptions options;
  WritableFile* const file;
  uint64_t offset = 0;
  Status status;
  BlockBuilder data_block;
  BlockBuilder index_block;
  std::unique_ptr<FilterBlockBuilder> filter_block;
  std::string last_key;
  uint64_t num_entries = 0;
  bool closed = false;  // Finish() or Abandon() has been called

  // The index entry for a data block is not written until the first key of
  // the next block is known, so that the entry can use a short separator
  // between the two instead of the block's full last key.
  //
  // True only when data_block is empty.
  bool pending_index_entry = false;
  BlockHandle pending_handle;  // the block the pending entry points at
};

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : rep_(std::make_unique<Rep>(options, file)) {}

TableBuilder::~TableBuilder() { assert(rep_->closed); }

void TableBuilder::Add(Slice key, Slice value) {
  Rep* r = rep_.get();
  assert(!r->closed);
  if (!r->ok()) return;
  assert(r->num_entries == 0 || r->options.comparator->Compare(key, r->last_key) > 0);

  if (r->pending_index_entry) {
    assert(r->data_block.empty());
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    r->pending_handle.EncodeTo(&handle_encoding);
    r->index_block.Add(r->last_key, handle_encoding);
    r->pending_index_entry = false;
  }

  if (r->filter_block != nullptr) {
    r->filter_block->AddKey(ExtractUserKey(key));
  }

  r->last_key.assign(key);
  r->num_entries++;
  r->data_block.Add(key, value);

  if (r->data_block.CurrentSizeEstimate() >= r->options.block_size) {
    Flush();
  }
}

void TableBuilder::Flush() {
  Rep* r = rep_.get();
  assert(!r->closed);
  if (!r->ok() || r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  r->WriteBlock(&r->data_block, &r->pending_handle);
  if (r->ok()) r->pending_index_entry = true;
}

Status TableBuilder::Finish() {
  Rep* r = rep_.get();
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_handle, metaindex_handle, index_handle;

  if (r->ok() && r->filter_block != nullptr) {
    r->WriteRawBlock(r->filter_block->Finish(), &filter_handle);
  }

  if (r->ok()) {
    BlockBuilder metaindex_block(r->options.block_restart_interval);
    if (r->filter_block != nullptr) {
      std::string handle_encoding;
      filter_handle.EncodeTo(&handle_encoding);
      metaindex_block.Add(kFilterMetaKey, handle_encoding);
    }
    r->WriteBlock(&metaindex_block, &metaindex_handle);
  }

  if (r->ok()) {
    if (r->pending_index_entry) {
      // No next block to separate from: any short key >= the last one works.
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, handle_encoding);
      r->pending_index_entry = false;
    }
    r->WriteBlock(&r->index_block, &index_handle);
  }

  if (r->ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_handle);
    footer.set_index_handle(index_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->ok()) r->offset += footer_encoding.size();
  }
  return r->status;
}

void TableBuilder::Abandon() {
  assert(!rep_->closed);
  rep_->closed = true;
}

Status TableBuilder::status() const { return rep_->status; }

uint64_t TableBuilder::NumEntries() const { return rep_->num_entries; }

uint64_t TableBuilder::FileSize() const { return rep_->offset; }

}  // namespace lsm
