#include "db/filename.h"

#include <charconv>
#include <cstdio>
#include <memory>

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

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return NumberedFileName(dbname, number, "dbtmp");
}

bool ParseFileName(Slice filename, uint64_t* number, FileType* type) {
  *number = 0;
  if (filename == "CURRENT") {
    *type = FileType::kCurrentFile;
    return true;
  }
  if (filename == "LOCK") {
    *type = FileType::kLockFile;
    return true;
  }

  const char* end = filename.data() + filename.size();
  if (filename.starts_with("MANIFEST-")) {
    const char* digits = filename.data() + 9;
    const auto [rest, ec] = std::from_chars(digits, end, *number);
    if (ec != std::errc() || rest == digits || rest != end) return false;
    *type = FileType::kDescriptorFile;
    return true;
  }

  // <decimal number>.<suffix>
  const auto [suffix, ec] = std::from_chars(filename.data(), end, *number);
  if (ec != std::errc() || suffix == filename.data()) return false;
  const Slice extension(suffix, end - suffix);
  if (extension == ".log") {
    *type = FileType::kLogFile;
  } else if (extension == ".sst") {
    *type = FileType::kTableFile;
  } else if (extension == ".dbtmp") {
    *type = FileType::kTempFile;
  } else {
    return false;
  }
  return true;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) return s;
  static constexpr size_t kBufferSize = 8192;
  const auto space = std::make_unique<char[]>(kBufferSize);
  while (true) {
    Slice fragment;
    s = file->Read(kBufferSize, &fragment, space.get());
    if (!s.ok() || fragment.empty()) break;
    data->append(fragment);
  }
  return s;
}

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number) {
  // CURRENT holds the manifest's file name relative to the directory.
  const std::string manifest = DescriptorFileName(dbname, descriptor_number);
  const std::string contents = manifest.substr(dbname.size() + 1) + "\n";
  const std::string tmp = TempFileName(dbname, descriptor_number);

  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(tmp, &file);
  if (s.ok()) s = file->Append(contents);
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  file.reset();
  if (s.ok()) s = env->RenameFile(tmp, CurrentFileName(dbname));
  if (s.ok()) s = env->SyncDir(dbname);
  if (!s.ok()) (void)env->RemoveFile(tmp);
  return s;
}

}  // namespace lsm
