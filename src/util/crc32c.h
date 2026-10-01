#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm::crc32c {

// CRC32C (Castagnoli) of concat(A, data[0, n)), where init_crc is the
// CRC32C of A.
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// CRCs are stored masked. Computing the CRC of bytes that themselves contain
// a CRC (a log record holding an SSTable block, say) has poor error-detection
// properties; masking the stored form avoids it.
constexpr uint32_t kMaskDelta = 0xa282ead8u;
inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }
inline uint32_t Unmask(uint32_t masked) {
  uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

namespace internal {
// Exposed so tests can check both paths against the same vectors.
uint32_t ExtendPortable(uint32_t init_crc, const char* data, size_t n);
// False when the CPU or the build lacks CRC32C instructions.
bool HardwareAvailable();
// Requires HardwareAvailable().
uint32_t ExtendHardware(uint32_t init_crc, const char* data, size_t n);
}  // namespace internal

}  // namespace lsm::crc32c
