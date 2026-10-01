#include "db/log_reader.h"

#include <algorithm>

#include "db/log_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm::log {

namespace {

// A record's header claims more payload than the file holds. Either the
// writer died mid-append (benign) or the length field is damaged and the
// real, shorter record is fully present. To tell them apart, look for a
// shorter length that satisfies the stored CRC and is followed by something
// a log could contain: the end of the file, a cut-off header, or a header
// with a valid type. A genuinely cut-off record passes this only by a CRC
// coincidence (2^-32 per candidate, and few candidates have a plausible
// successor).
bool LengthIsDamaged(const char* header, size_t available_payload) {
  const uint32_t expected = crc32c::Unmask(DecodeFixed32(header));
  const char* payload = header + kHeaderSize;
  uint32_t crc = crc32c::Value(header + 6, 1);  // the type byte
  for (size_t i = 0;; i++) {
    if (crc == expected) {
      const size_t rest = available_payload - i;
      if (rest < static_cast<size_t>(kHeaderSize)) return true;
      const int next_type = static_cast<uint8_t>(payload[i + 6]);
      if (next_type >= kFullType && next_type <= kMaxRecordType) return true;
    }
    if (i == available_payload) return false;
    crc = crc32c::Extend(crc, payload + i, 1);
  }
}

}  // namespace

Reader::Reader(SequentialFile* file) : file_(file), backing_store_(new char[kBlockSize]) {}

bool Reader::Fail(Slice reason) {
  status_ = Status::Corruption("log", reason);
  return false;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  scratch->clear();
  *record = Slice();
  if (!status_.ok()) return false;

  bool in_fragmented_record = false;
  while (true) {
    Slice fragment;
    switch (ReadPhysicalRecord(&fragment)) {
      case kFullType:
        if (in_fragmented_record) return Fail("FULL record inside a fragmented record");
        *record = fragment;
        return true;

      case kFirstType:
        if (in_fragmented_record) return Fail("FIRST record inside a fragmented record");
        scratch->assign(fragment);
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) return Fail("MIDDLE record without a FIRST");
        scratch->append(fragment);
        break;

      case kLastType:
        if (!in_fragmented_record) return Fail("LAST record without a FIRST");
        scratch->append(fragment);
        *record = *scratch;
        return true;

      case kEof:
        // Ending between fragments means the writer died partway through a
        // multi-block record.
        if (in_fragmented_record) tail_truncated_ = true;
        return false;

      case kError:
        return false;

      default:
        return Fail("unknown record type");
    }
  }
}

bool Reader::ReadBlock() {
  buffer_ = Slice();
  Status s = file_->Read(kBlockSize, &buffer_, backing_store_.get());
  if (!s.ok()) {
    buffer_ = Slice();
    status_ = s;
    return false;
  }
  if (buffer_.size() < static_cast<size_t>(kBlockSize)) eof_ = true;
  return true;
}

int Reader::ReadPhysicalRecord(Slice* result) {
  while (buffer_.size() < static_cast<size_t>(kHeaderSize)) {
    if (eof_) {
      // Leftover bytes at the end of the file are a header cut short.
      if (!buffer_.empty()) tail_truncated_ = true;
      buffer_ = Slice();
      return kEof;
    }
    // Whatever is left of a full block is the zero-filled trailer.
    if (!ReadBlock()) return kError;
  }

  const char* header = buffer_.data();
  const uint32_t length = static_cast<uint8_t>(header[4]) | (static_cast<uint8_t>(header[5]) << 8);
  const int type = static_cast<uint8_t>(header[6]);

  if (type == kZeroType && length == 0 && DecodeFixed32(header) == 0) {
    return ConsumeZeroTail();
  }

  if (kHeaderSize + length > buffer_.size()) {
    if (!eof_) {
      // A full block: the writer never lets a record cross the boundary.
      Fail("record length exceeds its block");
      return kError;
    }
    if (LengthIsDamaged(header, buffer_.size() - kHeaderSize)) {
      Fail("record length is damaged");
      return kError;
    }
    tail_truncated_ = true;
    buffer_ = Slice();
    return kEof;
  }

  const uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
  const uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
  if (actual_crc != expected_crc) {
    Fail("checksum mismatch");
    return kError;
  }

  buffer_.remove_prefix(kHeaderSize + length);
  *result = Slice(header + kHeaderSize, length);
  return type;
}

int Reader::ConsumeZeroTail() {
  // Some filesystems extend a file's size before its data reaches disk, so
  // power loss can leave a run of zeros where unsynced appends were. That is
  // a truncated tail. Zeros with live data after them are not.
  while (true) {
    if (std::any_of(buffer_.begin(), buffer_.end(), [](char c) { return c != 0; })) {
      Fail("zeroed region followed by data");
      return kError;
    }
    if (eof_) break;
    if (!ReadBlock()) return kError;
  }
  tail_truncated_ = true;
  buffer_ = Slice();
  return kEof;
}

}  // namespace lsm::log
