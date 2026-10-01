#include "util/crc32c.h"

#include <array>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#define LSM_CRC32C_X86 1
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define LSM_CRC32C_ARM 1
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#endif
#endif

namespace lsm::crc32c {

namespace {

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

namespace internal {

// ponytail: one byte per step. Only reached on CPUs without CRC32C
// instructions; switch to slicing-by-8 if such a CPU ever matters.
uint32_t ExtendPortable(uint32_t init_crc, const char* data, size_t n) {
  uint32_t crc = ~init_crc;
  for (size_t i = 0; i < n; i++) {
    crc = kTable[(crc ^ static_cast<uint8_t>(data[i])) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

#if defined(LSM_CRC32C_X86)

bool HardwareAvailable() { return __builtin_cpu_supports("sse4.2"); }

// ponytail: single stream, 8 bytes per instruction. Three interleaved
// streams would be about 3x faster on large buffers.
uint32_t ExtendHardware(uint32_t init_crc, const char* data, size_t n) {
  uint64_t crc = ~init_crc;
  for (; n >= 8; data += 8, n -= 8) {
    uint64_t word;
    std::memcpy(&word, data, 8);
    crc = _mm_crc32_u64(crc, word);
  }
  for (; n > 0; data++, n--) {
    crc = _mm_crc32_u8(static_cast<uint32_t>(crc), static_cast<uint8_t>(*data));
  }
  return ~static_cast<uint32_t>(crc);
}

#elif defined(LSM_CRC32C_ARM)

bool HardwareAvailable() {
#if defined(__linux__)
  return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
  return true;  // every Apple Silicon CPU has the CRC extension
#endif
}

// ponytail: single stream, as above.
uint32_t ExtendHardware(uint32_t init_crc, const char* data, size_t n) {
  uint32_t crc = ~init_crc;
  for (; n >= 8; data += 8, n -= 8) {
    uint64_t word;
    std::memcpy(&word, data, 8);
    crc = __crc32cd(crc, word);
  }
  for (; n > 0; data++, n--) {
    crc = __crc32cb(crc, static_cast<uint8_t>(*data));
  }
  return ~crc;
}

#else

bool HardwareAvailable() { return false; }
uint32_t ExtendHardware(uint32_t, const char*, size_t) { std::abort(); }

#endif

}  // namespace internal

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  static const bool hardware = internal::HardwareAvailable();
  return hardware ? internal::ExtendHardware(init_crc, data, n)
                  : internal::ExtendPortable(init_crc, data, n);
}

}  // namespace lsm::crc32c
