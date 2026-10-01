#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "lsm/env.h"

namespace lsm::fuzz {

// A property the fuzzer found a counterexample to. Traps, so that the
// fuzzer saves the input.
#define FUZZ_CHECK(condition)   \
  do {                          \
    if (!(condition)) {         \
      __builtin_trap();         \
    }                           \
  } while (0)

// Hands out the fuzzer's bytes a few at a time. Runs out gracefully: once
// the input is exhausted everything reads as zero.
class Input {
 public:
  Input(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool empty() const { return pos_ >= size_; }
  size_t remaining() const { return size_ - pos_; }

  uint8_t Byte() { return pos_ < size_ ? data_[pos_++] : 0; }
  uint32_t U16() {
    const uint32_t low = Byte();
    return low | (static_cast<uint32_t>(Byte()) << 8);
  }
  uint32_t U32() {
    const uint32_t low = U16();
    return low | (U16() << 16);
  }
  std::string Bytes(size_t n) {
    n = std::min(n, remaining());
    std::string out(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return out;
  }
  std::string Rest() { return Bytes(remaining()); }

 private:
  const uint8_t* const data_;
  const size_t size_;
  size_t pos_ = 0;
};

class StringSink : public WritableFile {
 public:
  Status Append(Slice data) override {
    contents.append(data);
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  std::string contents;
};

class StringSequentialFile : public SequentialFile {
 public:
  explicit StringSequentialFile(Slice contents) : contents_(contents) {}

  Status Read(size_t n, Slice* result, char* scratch) override {
    n = std::min(n, contents_.size());
    std::memcpy(scratch, contents_.data(), n);
    *result = Slice(scratch, n);
    contents_.remove_prefix(n);
    return Status::OK();
  }
  Status Skip(uint64_t n) override {
    contents_.remove_prefix(std::min<uint64_t>(n, contents_.size()));
    return Status::OK();
  }

 private:
  Slice contents_;
};

class StringRandomAccessFile : public RandomAccessFile {
 public:
  explicit StringRandomAccessFile(std::string contents) : contents_(std::move(contents)) {}

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    if (offset >= contents_.size()) {
      *result = Slice();
      return Status::OK();
    }
    n = std::min<uint64_t>(n, contents_.size() - offset);
    std::memcpy(scratch, contents_.data() + offset, n);
    *result = Slice(scratch, n);
    return Status::OK();
  }

 private:
  const std::string contents_;
};

// The three ways a file gets damaged, as chosen by the fuzzer.
//   0: left alone   1: cut short   2: one bit flipped   3: tail zeroed
struct Mutation {
  int kind = 0;
  size_t offset = 0;
  int bit = 0;
};

inline Mutation ReadMutation(Input* in, size_t file_size) {
  Mutation m;
  m.kind = in->Byte() % 4;
  m.offset = file_size == 0 ? 0 : in->U32() % file_size;
  m.bit = in->Byte() % 8;
  return m;
}

inline std::string Apply(const Mutation& m, std::string file) {
  if (file.empty()) return file;
  switch (m.kind) {
    case 1:
      file.resize(m.offset);
      break;
    case 2:
      file[m.offset] = static_cast<char>(file[m.offset] ^ (1 << m.bit));
      break;
    case 3:
      std::fill(file.begin() + static_cast<std::ptrdiff_t>(m.offset), file.end(), '\0');
      break;
    default:
      break;
  }
  return file;
}

}  // namespace lsm::fuzz
