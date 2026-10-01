#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/env.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// SSTable layout:
//
//   data block*  | filter block | metaindex block | index block | footer
//
// Every block is followed by a 5-byte trailer:
//
//   compression type: 1 byte
//   crc32c:           fixed32, masked; covers the block and the type byte
//
// The footer is the last 48 bytes of the file.

// Locates a block: its offset and size, not counting the trailer.
class BlockHandle {
 public:
  // Two varint64s.
  static constexpr size_t kMaxEncodedLength = 10 + 10;

  BlockHandle() = default;
  BlockHandle(uint64_t offset, uint64_t size) : offset_(offset), size_(size) {}

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }

  void EncodeTo(std::string* dst) const;
  // Parses from the front of *input and advances it.
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_ = 0;
  uint64_t size_ = 0;
};

constexpr size_t kBlockTrailerSize = 5;

// "LSMTABLE" read as a little-endian integer.
constexpr uint64_t kTableMagicNumber = 0x454c4241544d534cull;

//   metaindex handle | index handle | zero padding to 40 bytes | fixed64 magic
class Footer {
 public:
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  // input must be exactly the footer bytes.
  Status DecodeFrom(Slice input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

struct BlockContents {
  std::unique_ptr<char[]> data;
  size_t size = 0;
};

// Reads the block that handle names and verifies its checksum. Fails with
// Corruption if the handle points outside the file, the checksum does not
// match, or the block claims a compression type this build does not have.
Status ReadBlock(RandomAccessFile* file, uint64_t file_size, const BlockHandle& handle,
                 BlockContents* result);

}  // namespace lsm
