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

// An Env that models what survives power loss, and can fail on demand.
//
// POSIX makes two separate promises. Syncing a file makes its contents
// durable. Syncing a directory makes its entries durable: that a file was
// created in it, renamed within it, or removed from it. This Env tracks
// both, for every file written, created, renamed, or removed through it.
// SimulateCrash() then makes the real files match what a disk could hold
// after losing power:
//
//   - a rename whose directory was not synced since is undone;
//   - a file created in a directory that was not synced since is gone;
//   - a file removed from a directory that was not synced since may be
//     back (decided per file, at random);
//   - every other file is cut back to its synced length, optionally keeping
//     a random prefix of the unsynced tail (a torn write).
//
// Each file loses its tail independently, which also covers a filesystem
// persisting unsynced writes to different files out of order.
//
// Faults it can inject while the database runs:
//
//   - CrashAfter(n): the filesystem dies at the (n+1)-th mutating operation
//     from now, wherever in the engine that lands.
//   - SetErrorRate(p): any operation, reads included, fails with
//     probability p.
//   - SetDropDirSyncs(true): SyncDir reports success and does nothing.
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
    crash_countdown_ = -1;
  }

  bool IsFilesystemActive() {
    std::lock_guard lock(mutex_);
    return active_;
  }

  // The next n mutating operations succeed; the one after fails and leaves
  // the filesystem inactive.
  void CrashAfter(int64_t n) {
    std::lock_guard lock(mutex_);
    crash_countdown_ = n;
  }

  // Mutating operations performed so far. Lets a test size CrashAfter.
  uint64_t MutatingOps() {
    std::lock_guard lock(mutex_);
    return mutating_ops_;
  }

  // Each operation fails with this probability, independently. 0 disables.
  void SetErrorRate(double rate, uint32_t seed = 0) {
    std::lock_guard lock(mutex_);
    error_rate_ = rate;
    error_rng_.seed(seed);
  }

  uint64_t InjectedErrors() {
    std::lock_guard lock(mutex_);
    return injected_errors_;
  }

  // While set, SyncDir reports success without making anything durable.
  // Exists to show what goes wrong when a directory sync is skipped.
  void SetDropDirSyncs(bool drop) {
    std::lock_guard lock(mutex_);
    drop_dir_syncs_ = drop;
  }

  // REQUIRES: no file opened through this Env is still open for writing.
  // With rnd, each file keeps a random-length prefix of its unsynced tail
  // instead of losing all of it, and each unsynced removal is undone or not
  // at random. Without it, unsynced data is lost in full and removals stand.
  Status SimulateCrash(std::mt19937* rnd = nullptr) {
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

    for (const PendingRemoval& removal : pending_removals_) {
      const bool restore = rnd != nullptr && (*rnd)() % 2 == 0 && ::access(removal.name.c_str(), F_OK) != 0;
      if (restore) {
        ::rename(removal.backup.c_str(), removal.name.c_str());
      } else {
        ::unlink(removal.backup.c_str());
      }
    }
    pending_removals_.clear();

    for (auto& [fname, state] : files_) {
      struct stat st;
      if (::stat(fname.c_str(), &st) != 0) continue;  // removed behind our back
      const uint64_t on_disk = static_cast<uint64_t>(st.st_size);
      uint64_t keep = state.synced;
      if (rnd != nullptr && on_disk > keep) {
        keep += (*rnd)() % (on_disk - keep + 1);
      }
      keep = std::min(keep, on_disk);
      if (::truncate(fname.c_str(), static_cast<off_t>(keep)) != 0) {
        return Status::IOError(fname, "truncate failed");
      }
      state.synced = keep;
    }
    return Status::OK();
  }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    Status s = Intercept(fname, /*mutating=*/false);
    if (!s.ok()) return s;
    std::unique_ptr<SequentialFile> base;
    s = target()->NewSequentialFile(fname, &base);
    if (s.ok()) *result = std::make_unique<FaultySequentialFile>(this, fname, std::move(base));
    return s;
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    Status s = Intercept(fname, /*mutating=*/false);
    if (!s.ok()) return s;
    std::unique_ptr<RandomAccessFile> base;
    s = target()->NewRandomAccessFile(fname, &base);
    if (s.ok()) *result = std::make_unique<FaultyRandomAccessFile>(this, fname, std::move(base));
    return s;
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    Status s = Intercept(fname, /*mutating=*/true);
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
    Status s = Intercept(fname, /*mutating=*/true);
    if (!s.ok()) return s;
    std::lock_guard lock(mutex_);
    if (created_since_dir_sync_.count(fname) > 0) {
      // Never durably there, so there is nothing that could come back.
      s = target()->RemoveFile(fname);
      if (s.ok()) {
        files_.erase(fname);
        created_since_dir_sync_.erase(fname);
      }
      return s;
    }
    // Keep the file under another name until the directory is synced: the
    // removal is not durable before that, and a crash may bring it back.
    const std::string backup = fname + kRemovedMarker + std::to_string(next_backup_++);
    s = target()->RenameFile(fname, backup);
    if (s.ok()) {
      files_.erase(fname);
      pending_removals_.push_back({fname, backup});
    }
    return s;
  }

  Status RenameFile(const std::string& src, const std::string& dst) override {
    Status s = Intercept(src, /*mutating=*/true);
    if (!s.ok()) return s;
    std::lock_guard lock(mutex_);

    // Keep whatever dst names now reachable under another name, so that the
    // rename can be undone.
    PendingRename pending{src, dst, ""};
    if (target()->FileExists(dst)) {
      pending.backup = dst + kRenamedOverMarker + std::to_string(next_backup_++);
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
    Status s = Intercept(dir, /*mutating=*/true);
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
    std::erase_if(pending_removals_, [&](const PendingRemoval& r) {
      if (!r.name.starts_with(prefix)) return false;
      ::unlink(r.backup.c_str());
      return true;
    });
    return Status::OK();
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    Status s = Intercept(dir, /*mutating=*/false);
    if (!s.ok()) return s;
    s = target()->GetChildren(dir, result);
    // The backups are bookkeeping, not directory contents.
    std::erase_if(*result, [](const std::string& name) {
      return name.find(kRenamedOverMarker) != name.npos || name.find(kRemovedMarker) != name.npos;
    });
    return s;
  }

 private:
  static constexpr const char* kRenamedOverMarker = ".renamed-over-";
  static constexpr const char* kRemovedMarker = ".removed-";

  struct FileState {
    uint64_t synced = 0;
  };

  struct PendingRename {
    std::string src;
    std::string dst;
    std::string backup;  // where the file dst used to name is kept; empty if none
  };

  struct PendingRemoval {
    std::string name;
    std::string backup;  // where the removed file is kept
  };

  class TrackedFile : public WritableFile {
   public:
    TrackedFile(FaultInjectionEnv* env, std::string fname, std::unique_ptr<WritableFile> base)
        : env_(env), fname_(std::move(fname)), base_(std::move(base)) {}

    Status Append(Slice data) override {
      Status s = env_->Intercept(fname_, /*mutating=*/true);
      if (s.ok()) s = base_->Append(data);
      if (s.ok()) written_ += data.size();
      return s;
    }
    Status Flush() override {
      Status s = env_->Intercept(fname_, /*mutating=*/true);
      return s.ok() ? base_->Flush() : s;
    }
    Status Sync() override {
      Status s = env_->Intercept(fname_, /*mutating=*/true);
      // Get the bytes into the real file; skip the real fsync.
      if (s.ok()) s = base_->Flush();
      if (s.ok()) env_->RecordSync(fname_, written_);
      return s;
    }
    Status Close() override {
      Status s = env_->Intercept(fname_, /*mutating=*/true);
      // If this fails, base_ is destroyed without a flush and whatever it
      // had buffered is dropped, like any other unsynced data.
      return s.ok() ? base_->Close() : s;
    }

   private:
    FaultInjectionEnv* const env_;
    const std::string fname_;
    std::unique_ptr<WritableFile> base_;
    uint64_t written_ = 0;
  };

  class FaultySequentialFile : public SequentialFile {
   public:
    FaultySequentialFile(FaultInjectionEnv* env, std::string fname,
                         std::unique_ptr<SequentialFile> base)
        : env_(env), fname_(std::move(fname)), base_(std::move(base)) {}

    Status Read(size_t n, Slice* result, char* scratch) override {
      Status s = env_->Intercept(fname_, /*mutating=*/false);
      if (!s.ok()) {
        *result = Slice();
        return s;
      }
      return base_->Read(n, result, scratch);
    }
    Status Skip(uint64_t n) override { return base_->Skip(n); }

   private:
    FaultInjectionEnv* const env_;
    const std::string fname_;
    std::unique_ptr<SequentialFile> base_;
  };

  class FaultyRandomAccessFile : public RandomAccessFile {
   public:
    FaultyRandomAccessFile(FaultInjectionEnv* env, std::string fname,
                           std::unique_ptr<RandomAccessFile> base)
        : env_(env), fname_(std::move(fname)), base_(std::move(base)) {}

    Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
      Status s = env_->Intercept(fname_, /*mutating=*/false);
      if (!s.ok()) {
        *result = Slice();
        return s;
      }
      return base_->Read(offset, n, result, scratch);
    }

   private:
    FaultInjectionEnv* const env_;
    const std::string fname_;
    std::unique_ptr<RandomAccessFile> base_;
  };

  // Decides whether an operation goes ahead. Every operation passes
  // through here.
  Status Intercept(const std::string& context, bool mutating) {
    std::lock_guard lock(mutex_);
    if (mutating) {
      if (!active_) return Status::IOError(context, "simulated power loss");
      if (crash_countdown_ == 0) {
        active_ = false;
        crash_countdown_ = -1;
        return Status::IOError(context, "simulated power loss");
      }
      if (crash_countdown_ > 0) crash_countdown_--;
      mutating_ops_++;
    }
    if (error_rate_ > 0 &&
        std::uniform_real_distribution<double>(0, 1)(error_rng_) < error_rate_) {
      injected_errors_++;
      return Status::IOError(context, "injected I/O error");
    }
    return Status::OK();
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
  int64_t crash_countdown_ = -1;  // -1: no crash scheduled
  uint64_t mutating_ops_ = 0;
  double error_rate_ = 0;
  std::mt19937 error_rng_;
  uint64_t injected_errors_ = 0;
  bool drop_dir_syncs_ = false;
  std::map<std::string, FileState> files_;
  std::set<std::string> created_since_dir_sync_;
  std::vector<PendingRename> pending_renames_;
  std::vector<PendingRemoval> pending_removals_;
  int next_backup_ = 0;
};

}  // namespace lsm::test
