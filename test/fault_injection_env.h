#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "lsm/env.h"

namespace lsm::test {

// An Env that models what survives power loss.
//
// POSIX makes two separate promises. Syncing a file makes its contents
// durable. Syncing a directory makes its entries durable: that a file was
// created in it, or renamed within it. This Env tracks both, for every
// file written, created, or renamed through it. SimulateCrash() then makes
// the real files match what a disk could hold after losing power:
//
//   - a rename whose directory was not synced since is undone;
//   - a file created in a directory that was not synced since is gone;
//   - every other file is cut back to its synced length, optionally keeping
//     a random prefix of the unsynced tail (a torn write).
//
// Each file loses its tail independently, which also covers a filesystem
// persisting unsynced writes to different files out of order.
//
// Removals take effect at once. Real fsyncs are skipped: durability is
// simulated, and they are slow.
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

    // Undo renames, newest first.
    for (auto it = pending_renames_.rbegin(); it != pending_renames_.rend(); ++it) {
      if (::rename(it->dst.c_str(), it->src.c_str()) != 0) {
        return Status::IOError(it->dst, "could not undo rename");
      }
      MoveState(it->dst, it->src);
      if (!it->backup.empty()) {
        // dst had an earlier file under that name; it comes back.
        if (::rename(it->backup.c_str(), it->dst.c_str()) != 0) {
          return Status::IOError(it->backup, "could not restore renamed-over file");
        }
      }
    }
    pending_renames_.clear();

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
      state.synced = keep;
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

  Status RenameFile(const std::string& src, const std::string& dst) override {
    Status s = CheckActive(src);
    if (!s.ok()) return s;
    std::lock_guard lock(mutex_);

    // Keep whatever dst names now reachable under another name, so that the
    // rename can be undone.
    PendingRename pending{src, dst, ""};
    if (target()->FileExists(dst)) {
      pending.backup = dst + kBackupMarker + std::to_string(next_backup_++);
      if (::link(dst.c_str(), pending.backup.c_str()) != 0) {
        return Status::IOError(dst, "could not back up rename target");
      }
    }
    s = target()->RenameFile(src, dst);
    if (!s.ok()) {
      if (!pending.backup.empty()) ::unlink(pending.backup.c_str());
      return s;
    }
    MoveState(src, dst);
    pending_renames_.push_back(std::move(pending));
    return Status::OK();
  }

  Status SyncDir(const std::string& dir) override {
    Status s = CheckActive(dir);
    if (!s.ok()) return s;
    std::lock_guard lock(mutex_);
    if (drop_dir_syncs_) return Status::OK();

    const std::string prefix = dir + "/";
    std::erase_if(created_since_dir_sync_,
                  [&](const std::string& fname) { return fname.starts_with(prefix); });
    std::erase_if(pending_renames_, [&](const PendingRename& r) {
      if (!r.dst.starts_with(prefix)) return false;
      if (!r.backup.empty()) ::unlink(r.backup.c_str());
      return true;
    });
    return Status::OK();
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    Status s = target()->GetChildren(dir, result);
    // The backups of renamed-over files are bookkeeping, not directory
    // contents.
    std::erase_if(*result,
                  [](const std::string& name) { return name.find(kBackupMarker) != name.npos; });
    return s;
  }

 private:
  static constexpr const char* kBackupMarker = ".renamed-over-";

  struct FileState {
    uint64_t synced = 0;
  };

  struct PendingRename {
    std::string src;
    std::string dst;
    std::string backup;  // where the file dst used to name is kept; empty if none
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

  // REQUIRES: mutex_ held. The file known as from is now known as to.
  void MoveState(const std::string& from, const std::string& to) {
    if (auto it = files_.find(from); it != files_.end()) {
      files_[to] = it->second;
      files_.erase(from);
    } else {
      files_.erase(to);
    }
    const bool created = created_since_dir_sync_.erase(from) > 0;
    created_since_dir_sync_.erase(to);
    if (created) created_since_dir_sync_.insert(to);
  }

  void RecordSync(const std::string& fname, uint64_t written) {
    std::lock_guard lock(mutex_);
    if (auto it = files_.find(fname); it != files_.end()) {
      it->second.synced = written;
    }
  }

  std::mutex mutex_;
  bool active_ = true;
  bool drop_dir_syncs_ = false;
  std::map<std::string, FileState> files_;
  std::set<std::string> created_since_dir_sync_;
  std::vector<PendingRename> pending_renames_;
  int next_backup_ = 0;
};

}  // namespace lsm::test
