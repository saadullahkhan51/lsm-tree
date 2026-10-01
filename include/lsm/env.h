#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsm/slice.h"
#include "lsm/status.h"

namespace lsm {

// Read a file front to back. Not thread-safe.
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;

  // Reads up to n bytes. *result points into scratch (which must hold n
  // bytes) and is shorter than n only at end of file.
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

// Read a file at arbitrary offsets. Safe for concurrent use.
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;

  // Reads up to n bytes at offset. *result may point into scratch.
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

// Append-only file. Not thread-safe.
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  virtual Status Append(Slice data) = 0;
  // Hands buffered data to the OS. Survives a process crash, not power loss.
  virtual Status Flush() = 0;
  // Flush, then force the data to stable storage.
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

class FileLock {
 public:
  virtual ~FileLock() = default;
};

// Every file operation the engine performs goes through an Env, so that
// tests can substitute one that injects faults.
class Env {
 public:
  virtual ~Env() = default;

  // The POSIX implementation. Process-lifetime singleton; do not delete.
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;
  // Creates the file, truncating any existing one.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  // Names relative to dir, excluding "." and "..".
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;
  virtual Status RemoveFile(const std::string& fname) = 0;
  // Succeeds if the directory already exists.
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual Status RemoveDir(const std::string& dirname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;

  // Makes creations, renames, and removals inside dir durable. A rename or
  // a new file is not guaranteed to survive power loss until this returns.
  virtual Status SyncDir(const std::string& dir) = 0;

  // Takes an exclusive lock, failing if another holder exists (in this
  // process or another). Creates the file if missing.
  virtual Status LockFile(const std::string& fname, FileLock** lock) = 0;
  virtual Status UnlockFile(FileLock* lock) = 0;
};

}  // namespace lsm
