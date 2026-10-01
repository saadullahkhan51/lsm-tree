#include "db/filename.h"

#include <charconv>
#include <cstdio>

namespace lsm {

namespace {

std::string NumberedFileName(const std::string& dbname, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "sst");
}

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

bool ParseFileName(Slice filename, uint64_t* number, FileType* type) {
  if (filename == "LOCK") {
    *number = 0;
    *type = FileType::kLockFile;
    return true;
  }

  // <decimal number>.<suffix>
  const char* end = filename.data() + filename.size();
  const auto [suffix, ec] = std::from_chars(filename.data(), end, *number);
  if (ec != std::errc() || suffix == filename.data()) return false;
  const Slice extension(suffix, end - suffix);
  if (extension == ".log") {
    *type = FileType::kLogFile;
  } else if (extension == ".sst") {
    *type = FileType::kTableFile;
  } else {
    return false;
  }
  return true;
}

}  // namespace lsm
