#include "db/log_writer.h"

#include <cassert>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm::log {

Writer::Writer(WritableFile* dest) : dest_(dest) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    const char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Status Writer::AddRecord(Slice record) {
  const char* ptr = record.data();
  size_t left = record.size();

  // Fragment the record if necessary. An empty record still emits one
  // zero-length FULL record.
  Status s;
  bool begin = true;
  do {
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    if (leftover < kHeaderSize) {
      // Not even a header fits: zero-fill the trailer and start a new block.
      if (leftover > 0) {
        static_assert(kHeaderSize == 7);
        s = dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
        if (!s.ok()) return s;
      }
      block_offset_ = 0;
    }

    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length = left < avail ? left : avail;
    const bool end = left == fragment_length;

    RecordType type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType type, const char* ptr, size_t length) {
  assert(length <= 0xffff);
  assert(block_offset_ + kHeaderSize + length <= static_cast<size_t>(kBlockSize));

  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  buf[6] = static_cast<char>(type);
  EncodeFixed32(buf, crc32c::Mask(crc32c::Extend(type_crc_[type], ptr, length)));

  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) s = dest_->Append(Slice(ptr, length));
  // Hand the record to the OS before returning. Without this an
  // acknowledged write would sit in a user-space buffer and be lost if the
  // process were killed.
  if (s.ok()) s = dest_->Flush();
  block_offset_ += kHeaderSize + static_cast<int>(length);
  return s;
}

}  // namespace lsm::log
