#pragma once

#include <cstdint>
#include <string>

#include "lsm/env.h"
#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// Files in a database directory:
//   LOCK              held for as long as the database is open
//   CURRENT           names the live manifest
//   MANIFEST-NNNNNN   log of version edits
//   NNNNNN.log        write-ahead log
//   NNNNNN.sst        sorted table
//   NNNNNN.dbtmp      a file being written before it is renamed into place
enum class FileType { kLogFile, kLockFile, kTableFile, kDescriptorFile, kCurrentFile, kTempFile };

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string DescriptorFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);

// Recognizes a name (without directory) the engine created. *number is 0
// for types that carry none.
bool ParseFileName(Slice filename, uint64_t* number, FileType* type);

// Atomically points CURRENT at the manifest with the given number: writes
// the name to a temp file, syncs it, renames it over CURRENT, and syncs the
// directory. Without that last step the rename is not durable, and after a
// power failure CURRENT could name a manifest that no longer exists.
Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number);

Status ReadFileToString(Env* env, const std::string& fname, std::string* data);

}  // namespace lsm
