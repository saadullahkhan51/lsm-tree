#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm {

// Stable across platforms and releases: bloom filter bits on disk depend on it.
uint32_t Hash(const char* data, size_t n, uint32_t seed);

}  // namespace lsm
