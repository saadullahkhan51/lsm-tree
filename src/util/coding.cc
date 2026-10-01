#include "util/coding.h"

namespace lsm {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* EncodeVarint64(char* dst, uint64_t value) {
  while (value >= 0x80) {
    *dst++ = static_cast<char>(value | 0x80);
    value >>= 7;
  }
  *dst++ = static_cast<char>(value);
  return dst;
}

char* EncodeVarint32(char* dst, uint32_t value) { return EncodeVarint64(dst, value); }

void PutVarint32(std::string* dst, uint32_t value) { PutVarint64(dst, value); }

void PutVarint64(std::string* dst, uint64_t value) {
  char buf[10];
  dst->append(buf, EncodeVarint64(buf, value) - buf);
}

void PutLengthPrefixedSlice(std::string* dst, Slice value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

int VarintLength(uint64_t value) {
  int len = 1;
  while (value >= 0x80) {
    value >>= 7;
    len++;
  }
  return len;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = static_cast<uint8_t>(*p++);
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = static_cast<uint8_t>(*p++);
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* limit = input->data() + input->size();
  const char* q = GetVarint32Ptr(input->data(), limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, limit - q);
  return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  const char* limit = input->data() + input->size();
  const char* q = GetVarint64Ptr(input->data(), limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, limit - q);
  return true;
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  if (!GetVarint32(input, &len) || input->size() < len) return false;
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return true;
}

}  // namespace lsm
