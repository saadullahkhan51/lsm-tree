#pragma once

// The record format shared by the write-ahead log and the manifest.
//
// A log file is a sequence of 32 KiB blocks. Each block holds physical
// records:
//
//   crc32c: fixed32   masked; covers the type byte and the payload
//   length: fixed16   payload bytes
//   type:   1 byte
//   payload
//
// A logical record that does not fit in the rest of a block is split into
// FIRST, MIDDLE..., LAST fragments; one that fits is a single FULL record.
// A physical record never spans a block boundary. If fewer than 7 bytes
// remain in a block they are zero-filled and skipped.

namespace lsm::log {

enum RecordType {
  kZeroType = 0,  // never written; what a zero-filled region reads as
  kFullType = 1,
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};
constexpr int kMaxRecordType = kLastType;

constexpr int kBlockSize = 32768;
constexpr int kHeaderSize = 4 + 2 + 1;

}  // namespace lsm::log
