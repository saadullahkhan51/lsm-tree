#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include "lsm/slice.h"

namespace lsm {

// Every on-disk integer is little-endian; fixed-width codecs are a memcpy.
static_assert(std::endian::native == std::endian::little);

inline void EncodeFixed32(char* dst, uint32_t value) { std::memcpy(dst, &value, sizeof(value)); }
inline void EncodeFixed64(char* dst, uint64_t value) { std::memcpy(dst, &value, sizeof(value)); }

inline uint32_t DecodeFixed32(const char* ptr) {
  uint32_t value;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}
inline uint64_t DecodeFixed64(const char* ptr) {
  uint64_t value;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
// varint32 length followed by the bytes.
void PutLengthPrefixedSlice(std::string* dst, Slice value);

// Write a varint at dst (5 or 10 bytes must be available) and return the
// position just past it.
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// Parse from the front of *input and advance it. Return false, leaving
// *input unspecified, on truncated or overlong input.
bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// Parse from [p, limit). Return the position past the varint, or nullptr on
// truncated or overlong input.
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

int VarintLength(uint64_t value);

}  // namespace lsm
