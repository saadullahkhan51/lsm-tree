#pragma once

#include <string_view>

namespace lsm {

// A non-owning view of bytes. std::string_view compares as unsigned char,
// which is the bytewise order the engine relies on.
using Slice = std::string_view;

}  // namespace lsm
