#include "table/format.h"

#include <cassert>
#include <cstring>

#include "lsm/options.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength);  // zero padding
  PutFixed64(dst, kTableMagicNumber);
  assert(dst->size() == original_size + kEncodedLength);
}

Status Footer::DecodeFrom(Slice input) {
  if (input.size() != kEncodedLength) {
    return Status::Corruption("bad sstable footer length");
  }
  if (DecodeFixed64(input.data() + kEncodedLength - 8) != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  Status s = metaindex_handle_.DecodeFrom(&input);
  if (s.ok()) s = index_handle_.DecodeFrom(&input);
  return s;
}

Status ReadBlock(RandomAccessFile* file, uint64_t file_size, const BlockHandle& handle,
                 BlockContents* result) {
  result->data.reset();
  result->size = 0;

  // The handle may come from a damaged footer or index. Check it against
  // the file before trusting it with an allocation.
  if (handle.offset() > file_size || handle.size() > file_size ||
      handle.offset() + handle.size() + kBlockTrailerSize > file_size) {
    return Status::Corruption("block handle points outside the file");
  }

  const size_t n = static_cast<size_t>(handle.size());
  auto buf = std::make_unique_for_overwrite<char[]>(n + kBlockTrailerSize);
  Slice contents;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf.get());
  if (!s.ok()) return s;
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  const char* data = contents.data();
  const uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
  if (crc32c::Value(data, n + 1) != expected_crc) {
    return Status::Corruption("block checksum mismatch");
  }
  if (data[n] != kNoCompression) {
    return Status::Corruption("unsupported block compression type");
  }

  if (data != buf.get()) {
    // The file handed back its own memory instead of filling our buffer.
    std::memcpy(buf.get(), data, n);
  }
  result->data = std::move(buf);
  result->size = n;
  return Status::OK();
}

}  // namespace lsm
