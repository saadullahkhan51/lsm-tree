#pragma once

#include <cstdint>

#include "db/dbformat.h"

namespace lsm {

// What the database knows about one SSTable without opening it.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  InternalKey smallest;  // smallest internal key in the table
  InternalKey largest;   // largest internal key in the table
};

}  // namespace lsm
