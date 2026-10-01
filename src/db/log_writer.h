#pragma once

#include <cstdint>

#include "db/log_format.h"
#include "lsm/env.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm::log {

class Writer {
 public:
  // dest must be empty and must outlive the Writer.
  explicit Writer(WritableFile* dest);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Appends one logical record and flushes it to the OS, so it survives a
  // process crash. It is on stable storage only after dest->Sync().
  Status AddRecord(Slice record);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_ = 0;  // current offset in the block

  // CRC of each type byte, precomputed so that a record's CRC only has to
  // be extended over its payload.
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace lsm::log
