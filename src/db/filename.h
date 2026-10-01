#pragma once

#include <cstdint>
#include <string>

#include "lsm/slice.h"

namespace lsm {

// Files in a database directory:
//   LOCK          held for as long as the database is open
//   NNNNNN.log    write-ahead log
enum class FileType { kLogFile, kLockFile };

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string LockFileName(const std::string& dbname);

// Recognizes a name (without directory) the engine created. *number is 0
// for types that carry none.
bool ParseFileName(Slice filename, uint64_t* number, FileType* type);

}  // namespace lsm
