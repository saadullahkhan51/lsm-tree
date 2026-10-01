#pragma once

#include <memory>
#include <string>

#include "lsm/env.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm::log {

// Reads the logical records of a log file in order.
//
// The reader separates the two ways a log can end badly:
//
//   - An incomplete final record. This is what a crash in the middle of an
//     append leaves behind; the record was never acknowledged. It is a clean
//     end of log: ReadRecord returns false, status() is OK, and
//     tail_truncated() is true.
//   - Damage: a checksum mismatch, an impossible length, or a fragment that
//     cannot follow the one before it. ReadRecord returns false and status()
//     is Corruption. Nothing after the damage is returned, because skipping
//     a record would leave a hole in the recovered history.
class Reader {
 public:
  // file must outlive the Reader and be positioned at the start of the log.
  explicit Reader(SequentialFile* file);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Reads the next record into *record, which stays valid until the next
  // call or until *scratch is modified. Returns false when there are no more
  // records; status() then tells why.
  bool ReadRecord(Slice* record, std::string* scratch);

  // OK after a clean end of log (including a truncated tail); otherwise the
  // error that stopped reading. Sticky.
  const Status& status() const { return status_; }

  // True if the log ended inside a record rather than on a record boundary.
  bool tail_truncated() const { return tail_truncated_; }

 private:
  // Extra values ReadPhysicalRecord returns alongside RecordType. Outside
  // the range of a byte, so no type read from a file can be mistaken for one.
  enum { kEof = 256, kError = 257 };

  int ReadPhysicalRecord(Slice* result);
  bool Fail(Slice reason);
  // Reads the next block into buffer_. Returns false on a read error.
  bool ReadBlock();
  // Called on a zero header: clean end of log if everything through the
  // end of the file is zero, corruption otherwise.
  int ConsumeZeroTail();

  SequentialFile* const file_;
  const std::unique_ptr<char[]> backing_store_;
  Slice buffer_;      // unconsumed bytes of the current block
  bool eof_ = false;  // the last read returned less than a full block
  bool tail_truncated_ = false;
  Status status_;
};

}  // namespace lsm::log
