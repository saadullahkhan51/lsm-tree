// The bit-flip harness. Builds a database with data in every kind of file
// (tables on several levels, a write-ahead log with unflushed writes, a
// manifest, CURRENT), then, many times over: copies it, flips one bit in
// one file, opens the copy, and reads every key.
//
// Each read must come back with the right answer or an explicit Corruption
// status. A wrong value, a missing key, or a deleted key that is back, all
// reported as success, fails the test.
//
//   LSM_BITFLIP_PER_FILE   bits flipped per file (default 40; files with
//                          no more bits than that are covered in full)
//   LSM_BITFLIP_SEED       seed for choosing the bits (default 1)

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/db_impl.h"
#include "db/filename.h"
#include "fault_injection_env.h"
#include "gtest/gtest.h"
#include "lsm/db.h"
#include "testutil.h"

namespace lsm {

namespace {

namespace fs = std::filesystem;

constexpr int kKeySpace = 400;

uint64_t EnvOr(const char* name, uint64_t fallback) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::strtoull(value, nullptr, 10) : fallback;
}

std::string Key(int k) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%04d", k);
  return buf;
}

Options SmallOptions(Env* env) {
  Options options;
  options.env = env;
  options.write_buffer_size = 16 << 10;
  options.max_file_size = 8 << 10;
  options.max_bytes_for_level_base = 32 << 10;
  options.level_size_multiplier = 4;
  options.block_size = 1024;
  return options;
}

const char* TypeName(FileType type) {
  switch (type) {
    case FileType::kLogFile:
      return "write-ahead log";
    case FileType::kTableFile:
      return "table";
    case FileType::kDescriptorFile:
      return "manifest";
    case FileType::kCurrentFile:
      return "CURRENT";
    default:
      return "other";
  }
}

struct Tally {
  uint64_t trials = 0;
  uint64_t rejected_at_open = 0;  // DB::Open returned Corruption
  uint64_t rejected_on_read = 0;  // some read returned Corruption; no read lied
  uint64_t harmless = 0;          // every read was correct
};

}  // namespace

TEST(BitFlip, EveryReadIsCorrectOrReportsCorruption) {
  const uint64_t per_file = EnvOr("LSM_BITFLIP_PER_FILE", 40);
  std::mt19937 rnd(static_cast<uint32_t>(EnvOr("LSM_BITFLIP_SEED", 1)));
  const std::string pristine = test::NewTestDir("bitflip_pristine");
  const std::string trial_dir = test::NewTestDir("bitflip_trial");

  // Build the database, and the map of what it should contain.
  std::map<std::string, std::string> model;
  {
    test::FaultInjectionEnv env(Env::Default());  // skips real fsyncs
    DB* raw = nullptr;
    ASSERT_OK(DB::Open(SmallOptions(&env), pristine, &raw));
    std::unique_ptr<DB> db(raw);
    auto put = [&](int k, int version) {
      const std::string value =
          std::to_string(version) + ":" + std::string(50 + (k * 7 + version) % 400, 'a' + k % 26);
      ASSERT_OK(db->Put(WriteOptions(), Key(k), value));
      model[Key(k)] = value;
    };
    auto del = [&](int k) {
      ASSERT_OK(db->Delete(WriteOptions(), Key(k)));
      model.erase(Key(k));
    };

    for (int k = 0; k < kKeySpace; k++) put(k, 1);
    ASSERT_OK(db->CompactRange(nullptr, nullptr));  // a deep level
    for (int k = 0; k < kKeySpace; k += 2) put(k, 2);
    for (int k = 0; k < kKeySpace; k += 5) del(k);
    ASSERT_OK(static_cast<DBImpl*>(db.get())->TEST_WaitForBackgroundWork());
    for (int k = 1; k < kKeySpace; k += 3) put(k, 3);  // upper levels
    ASSERT_OK(static_cast<DBImpl*>(db.get())->TEST_FlushMemTable());
    ASSERT_OK(static_cast<DBImpl*>(db.get())->TEST_WaitForBackgroundWork());
    // Left in the log only: values, overwrites, and deletes of table data.
    for (int k = 0; k < 40; k++) put(k * 9 % kKeySpace, 4);
    for (int k = 0; k < 15; k++) del(k * 13 % kKeySpace);

    std::string levels;
    db->GetProperty("lsm.levels", &levels);
    std::printf("  database under test:\n%s", levels.c_str());
  }

  // The files to damage.
  struct Target {
    std::string name;
    FileType type;
    uint64_t size;
  };
  std::vector<Target> targets;
  std::set<FileType> types_present;
  for (const auto& entry : fs::directory_iterator(pristine)) {
    uint64_t number;
    FileType type;
    const std::string name = entry.path().filename().string();
    if (!ParseFileName(name, &number, &type) || type == FileType::kLockFile) continue;
    if (entry.file_size() == 0) continue;
    targets.push_back({name, type, entry.file_size()});
    types_present.insert(type);
  }
  std::sort(targets.begin(), targets.end(),
            [](const Target& a, const Target& b) { return a.name < b.name; });
  for (FileType required : {FileType::kLogFile, FileType::kTableFile, FileType::kDescriptorFile,
                            FileType::kCurrentFile}) {
    ASSERT_TRUE(types_present.count(required)) << "no " << TypeName(required) << " to damage";
  }

  std::map<FileType, Tally> tallies;
  for (const Target& target : targets) {
    // Which bits: all of them for a small file, a random sample otherwise.
    const uint64_t total_bits = target.size * 8;
    std::set<uint64_t> bits;
    if (total_bits <= per_file) {
      for (uint64_t b = 0; b < total_bits; b++) bits.insert(b);
    } else {
      while (bits.size() < per_file) bits.insert(rnd() % total_bits);
    }

    for (uint64_t bit : bits) {
      SCOPED_TRACE(target.name + " (" + TypeName(target.type) + "), byte " +
                   std::to_string(bit / 8) + " bit " + std::to_string(bit % 8));
      Tally& tally = tallies[target.type];
      tally.trials++;

      // A fresh copy with one bit flipped.
      fs::remove_all(trial_dir);
      fs::copy(pristine, trial_dir, fs::copy_options::recursive);
      {
        std::fstream f(trial_dir + "/" + target.name,
                       std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(static_cast<std::streamoff>(bit / 8));
        char byte;
        f.get(byte);
        byte = static_cast<char>(byte ^ (1 << (bit % 8)));
        f.seekp(static_cast<std::streamoff>(bit / 8));
        f.put(byte);
      }

      test::FaultInjectionEnv env(Env::Default());
      DB* raw = nullptr;
      Status s = DB::Open(SmallOptions(&env), trial_dir, &raw);
      std::unique_ptr<DB> db(raw);
      if (!s.ok()) {
        ASSERT_TRUE(s.IsCorruption()) << "open failed, but not with Corruption: " << s.ToString();
        tally.rejected_at_open++;
        continue;
      }

      bool saw_corruption = false;
      // Point reads of every key, present or not.
      for (int k = 0; k < kKeySpace; k++) {
        const std::string key = Key(k);
        std::string value;
        s = db->Get(ReadOptions(), key, &value);
        auto it = model.find(key);
        if (s.ok()) {
          ASSERT_TRUE(it != model.end()) << key << ": a deleted or never-written key was read";
          ASSERT_EQ(it->second, value) << key << ": wrong value returned as success";
        } else if (s.IsNotFound()) {
          ASSERT_TRUE(it == model.end()) << key << ": an existing key was reported missing";
        } else {
          ASSERT_TRUE(s.IsCorruption()) << key << ": " << s.ToString();
          saw_corruption = true;
        }
      }
      // And a scan. If it reports success it must have seen everything.
      {
        std::unique_ptr<Iterator> iter(db->NewIterator(ReadOptions()));
        std::map<std::string, std::string> scanned;
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
          scanned.emplace(iter->key(), iter->value());
        }
        if (iter->status().ok()) {
          ASSERT_EQ(model, scanned) << "scan returned wrong contents as success";
        } else {
          ASSERT_TRUE(iter->status().IsCorruption()) << iter->status().ToString();
          saw_corruption = true;
        }
      }

      if (saw_corruption) {
        tally.rejected_on_read++;
      } else {
        tally.harmless++;
      }
    }
  }

  std::printf("\n  %-16s %8s %12s %12s %10s\n", "file type", "flips", "open failed", "read failed",
              "harmless");
  for (const auto& [type, t] : tallies) {
    std::printf("  %-16s %8llu %12llu %12llu %10llu\n", TypeName(type),
                static_cast<unsigned long long>(t.trials),
                static_cast<unsigned long long>(t.rejected_at_open),
                static_cast<unsigned long long>(t.rejected_on_read),
                static_cast<unsigned long long>(t.harmless));
  }
  std::printf("  (harmless: every read after the flip still returned correct data)\n\n");

  // Everything in a log or a manifest is inside a checksummed record, and
  // both are read in full at open.
  EXPECT_EQ(tallies[FileType::kLogFile].trials, tallies[FileType::kLogFile].rejected_at_open);
  EXPECT_EQ(tallies[FileType::kDescriptorFile].trials,
            tallies[FileType::kDescriptorFile].rejected_at_open);
  EXPECT_EQ(tallies[FileType::kCurrentFile].trials,
            tallies[FileType::kCurrentFile].rejected_at_open);
  // Tables are read lazily, so damage shows up on the reads that touch it.
  EXPECT_GT(tallies[FileType::kTableFile].rejected_on_read, 0u);
}

}  // namespace lsm
