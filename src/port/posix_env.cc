#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "lsm/env.h"

namespace lsm {

namespace {

Status PosixError(const std::string& context, int err) {
  if (err == ENOENT) {
    return Status::NotFound(context, std::strerror(err));
  }
  return Status::IOError(context, std::strerror(err));
}

// Forces fd's contents to stable storage.
Status SyncFd(int fd, const std::string& name, [[maybe_unused]] bool is_dir) {
#if defined(__APPLE__)
  // fsync on macOS leaves data in the drive's write cache; F_FULLFSYNC
  // flushes it. Fall back to fsync on filesystems that do not support it.
  if (::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0) return Status::OK();
#else
  // fdatasync skips metadata that is not needed to read the data back
  // (mtime), but does persist a changed file size.
  if ((is_dir ? ::fsync(fd) : ::fdatasync(fd)) == 0) return Status::OK();
#endif
  return PosixError(name, errno);
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, Slice* result, char* scratch) override {
    while (true) {
      ssize_t r = ::read(fd_, scratch, n);
      if (r >= 0) {
        *result = Slice(scratch, r);
        return Status::OK();
      }
      if (errno != EINTR) {
        *result = Slice();
        return PosixError(name_, errno);
      }
    }
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == -1) {
      return PosixError(name_, errno);
    }
    return Status::OK();
  }

 private:
  const std::string name_;
  const int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    while (true) {
      ssize_t r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
      if (r >= 0) {
        *result = Slice(scratch, r);
        return Status::OK();
      }
      if (errno != EINTR) {
        *result = Slice();
        return PosixError(name_, errno);
      }
    }
  }

 private:
  const std::string name_;
  const int fd_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) ::close(fd_);  // not closed by the owner; buffered data is dropped
  }

  Status Append(Slice data) override {
    // Fill the buffer; anything that does not fit goes after a flush.
    const size_t copy = std::min(data.size(), kBufferSize - pos_);
    std::memcpy(buf_ + pos_, data.data(), copy);
    pos_ += copy;
    data.remove_prefix(copy);
    if (data.empty()) return Status::OK();

    Status s = Flush();
    if (!s.ok()) return s;
    if (data.size() < kBufferSize) {
      std::memcpy(buf_, data.data(), data.size());
      pos_ = data.size();
      return Status::OK();
    }
    return WriteAll(data.data(), data.size());
  }

  Status Flush() override {
    Status s = WriteAll(buf_, pos_);
    pos_ = 0;
    return s;
  }

  Status Sync() override {
    Status s = Flush();
    if (!s.ok()) return s;
    return SyncFd(fd_, name_, /*is_dir=*/false);
  }

  Status Close() override {
    Status s = Flush();
    if (::close(fd_) != 0 && s.ok()) {
      s = PosixError(name_, errno);
    }
    fd_ = -1;
    return s;
  }

 private:
  static constexpr size_t kBufferSize = 65536;

  Status WriteAll(const char* data, size_t n) {
    while (n > 0) {
      ssize_t w = ::write(fd_, data, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        return PosixError(name_, errno);
      }
      data += w;
      n -= w;
    }
    return Status::OK();
  }

  const std::string name_;
  int fd_;
  size_t pos_ = 0;
  char buf_[kBufferSize];
};

struct PosixFileLock final : public FileLock {
  int fd;
  std::string name;
};

class PosixEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      result->reset();
      return PosixError(fname, errno);
    }
    *result = std::make_unique<PosixSequentialFile>(fname, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      result->reset();
      return PosixError(fname, errno);
    }
    *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    int fd = ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      result->reset();
      return PosixError(fname, errno);
    }
    *result = std::make_unique<PosixWritableFile>(fname, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override { return ::access(fname.c_str(), F_OK) == 0; }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return PosixError(dir, errno);
    while (struct dirent* entry = ::readdir(d)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") result->push_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    return ::unlink(fname.c_str()) == 0 ? Status::OK() : PosixError(fname, errno);
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) == 0 || errno == EEXIST) return Status::OK();
    return PosixError(dirname, errno);
  }

  Status RemoveDir(const std::string& dirname) override {
    return ::rmdir(dirname.c_str()) == 0 ? Status::OK() : PosixError(dirname, errno);
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat st;
    if (::stat(fname.c_str(), &st) != 0) {
      *size = 0;
      return PosixError(fname, errno);
    }
    *size = static_cast<uint64_t>(st.st_size);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    return ::rename(src.c_str(), target.c_str()) == 0 ? Status::OK() : PosixError(src, errno);
  }

  Status SyncDir(const std::string& dir) override {
    int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(dir, errno);
    Status s = SyncFd(fd, dir, /*is_dir=*/true);
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = nullptr;
    int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(fname, errno);
    // flock locks belong to the open file description, so a second open in
    // this same process conflicts too.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      Status s = PosixError("lock " + fname, errno);
      ::close(fd);
      return s;
    }
    auto* posix_lock = new PosixFileLock;
    posix_lock->fd = fd;
    posix_lock->name = fname;
    *lock = posix_lock;
    return Status::OK();
  }

  Status UnlockFile(FileLock* lock) override {
    auto* posix_lock = static_cast<PosixFileLock*>(lock);
    Status s;
    if (::flock(posix_lock->fd, LOCK_UN) != 0) {
      s = PosixError("unlock " + posix_lock->name, errno);
    }
    ::close(posix_lock->fd);
    delete posix_lock;
    return s;
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

}  // namespace lsm
