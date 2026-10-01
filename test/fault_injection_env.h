#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>

#include "lsm/env.h"

namespace lsm::test {

// An Env that models what survives power loss.
//
// It tracks, for every file written through it, how many bytes have been
// synced, and for every file created through it, whether the directory has
// been synced since. SimulateCrash() then makes the real files match what a
// disk would hold after losing power:
//
//   - a file whose directory entry was never synced is gone;
//   - every other file is cut back to its synced length, optionally keeping
//     a random prefix of the unsynced tail (a torn write).
//
// Each file loses its tail independently, which also covers the case where
// the filesystem persisted unsynced writes to different files out of order.
//
// Real fsyncs are skipped: durability is simulated, and they are slow.
class FaultInjectionEnv : public EnvWrapper {
 public:
  explicit FaultInjectionEnv(Env* base) : EnvWrapper(base) {}

  // While inactive every write, sync, create, rename, and remove fails, as
  // if the machine had already lost power. Reads still work.
  void SetFilesystemActive(bool active) {
    std::lock_guard lock(mutex_);
    active_ = active;
  }

  // While set, SyncDir reports success without making anything durable.
  // Exists to show what goes wrong when a directory sync is skipped.
  void SetDropDirSyncs(bool drop) {
    std::lock_guard lock(mutex_);
    drop_dir_syncs_ = drop;
  }

  // REQUIRES: no file opened through this Env is still open for writing.
  // If torn is non-null, each file keeps a random-length prefix of its
  // unsynced tail instead of losing all of it.
  Status SimulateCrash(std::mt19937* torn = nullptr) {
    std::lock_guard lock(mutex_);
    for (const std::string& fname : created_since_dir_sync_) {
      ::unlink(fname.c_str());
      files_.erase(fname);
    }
    created_since_dir_sync_.clear();

    for (auto& [fname, state] : files_) {
      struct stat st;
      if (::stat(fname.c_str(), &st) != 0) continue;  // removed behind our back
      const uint64_t on_disk = static_cast<uint64_t>(st.st_size);
      uint64_t keep = state.synced;
      if (torn != nullptr && on_disk > keep) {
        keep += (*torn)() % (on_disk - keep + 1);
      }
      keep = std::min(keep, on_disk);
      if (::truncate(fname.c_str(), static_cast<off_t>(keep)) != 0) {
        return Status::IOError(fname, "truncate failed");
      }
      state.written = state.synced = keep;
    }
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    Status s = CheckActive(fname);
    if (!s.ok()) return s;
    std::unique_ptr<WritableFile> base;
    s = target()->NewWritableFile(fname, &base);
    if (!s.ok()) return s;
    {
      std::lock_guard lock(mutex_);
      files_[fname] = FileState{};
      created_since_dir_sync_.insert(fname);
    }
    *result = std::make_unique<TrackedFile>(this, fname, std::move(base));
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    Status s = CheckActive(fname);
    if (!s.ok()) return s;
    s = target()->RemoveFile(fname);
    if (s.ok()) {
      std::lock_guard lock(mutex_);
      files_.erase(fname);
      created_since_dir_sync_.erase(fname);
    }
    return s;
  }

  Status SyncDir(const std::string& dir) override {
    Status s = CheckActive(dir);
    if (!s.ok()) return s;
    std::lock_guard lock(mutex_);
    if (drop_dir_syncs_) return Status::OK();
    std::erase_if(created_since_dir_sync_,
                  [&](const std::string& fname) { return fname.starts_with(dir + "/"); });
    return Status::OK();
  }

 private:
  struct FileState {
    uint64_t written = 0;
    uint64_t synced = 0;
  };

  class TrackedFile : public WritableFile {
   public:
    TrackedFile(FaultInjectionEnv* env, std::string fname, std::unique_ptr<WritableFile> base)
        : env_(env), fname_(std::move(fname)), base_(std::move(base)) {}

    Status Append(Slice data) override {
      Status s = env_->CheckActive(fname_);
      if (s.ok()) s = base_->Append(data);
      if (s.ok()) written_ += data.size();
      return s;
    }
    Status Flush() override {
      Status s = env_->CheckActive(fname_);
      return s.ok() ? base_->Flush() : s;
    }
    Status Sync() override {
      Status s = Flush();  // get the bytes into the real file; skip the real fsync
      if (s.ok()) env_->RecordSync(fname_, written_);
      return s;
    }
    Status Close() override {
      Status s = env_->CheckActive(fname_);
      // When the filesystem is down, base_ is destroyed without a flush and
      // whatever it had buffered is dropped, like any other unsynced data.
      return s.ok() ? base_->Close() : s;
    }

   private:
    FaultInjectionEnv* const env_;
    const std::string fname_;
    std::unique_ptr<WritableFile> base_;
    uint64_t written_ = 0;
  };

  Status CheckActive(const std::string& context) {
    std::lock_guard lock(mutex_);
    return active_ ? Status::OK() : Status::IOError(context, "simulated power loss");
  }

  void RecordSync(const std::string& fname, uint64_t written) {
    std::lock_guard lock(mutex_);
    if (auto it = files_.find(fname); it != files_.end()) {
      it->second.written = it->second.synced = written;
    }
  }

  std::mutex mutex_;
  bool active_ = true;
  bool drop_dir_syncs_ = false;
  std::map<std::string, FileState> files_;
  std::set<std::string> created_since_dir_sync_;
};

}  // namespace lsm::test
