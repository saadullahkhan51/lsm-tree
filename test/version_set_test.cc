#include "db/version_set.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "db/filename.h"
#include "db/log_writer.h"
#include "db/table_cache.h"
#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

// --- FindFile ---

class FindFileTest : public testing::Test {
 protected:
  ~FindFileTest() override {
    for (FileMetaData* f : files_) delete f;
  }

  void Add(const char* smallest, const char* largest, SequenceNumber smallest_seq = 100,
           SequenceNumber largest_seq = 100) {
    auto* f = new FileMetaData;
    f->number = files_.size() + 1;
    f->smallest = InternalKey(smallest, smallest_seq, kTypeValue);
    f->largest = InternalKey(largest, largest_seq, kTypeValue);
    files_.push_back(f);
  }

  size_t Find(const char* key) {
    const InternalKey target(key, 100, kTypeValue);
    return FindFile(InternalKeyComparator(BytewiseComparator()), files_, target.Encode());
  }

  std::vector<FileMetaData*> files_;
};

TEST_F(FindFileTest, Empty) { ASSERT_EQ(0u, Find("foo")); }

TEST_F(FindFileTest, Single) {
  Add("p", "q");
  ASSERT_EQ(0u, Find("a"));
  ASSERT_EQ(0u, Find("p"));
  ASSERT_EQ(0u, Find("p1"));
  ASSERT_EQ(0u, Find("q"));
  ASSERT_EQ(1u, Find("q1"));
  ASSERT_EQ(1u, Find("z"));
}

TEST_F(FindFileTest, Multiple) {
  Add("150", "200");
  Add("200", "250");  // shares a user key boundary, as internal keys allow
  Add("300", "350");
  Add("400", "450");
  ASSERT_EQ(0u, Find("100"));
  ASSERT_EQ(0u, Find("150"));
  ASSERT_EQ(0u, Find("151"));
  ASSERT_EQ(0u, Find("199"));
  ASSERT_EQ(0u, Find("200"));
  ASSERT_EQ(1u, Find("201"));
  ASSERT_EQ(1u, Find("249"));
  ASSERT_EQ(1u, Find("250"));
  ASSERT_EQ(2u, Find("251"));
  ASSERT_EQ(2u, Find("299"));
  ASSERT_EQ(2u, Find("300"));
  ASSERT_EQ(2u, Find("349"));
  ASSERT_EQ(2u, Find("350"));
  ASSERT_EQ(3u, Find("351"));
  ASSERT_EQ(3u, Find("400"));
  ASSERT_EQ(3u, Find("450"));
  ASSERT_EQ(4u, Find("451"));
}

TEST_F(FindFileTest, SequenceDecidesWithinAUserKey) {
  // Largest key is ("k", seq 50). A lookup at a newer sequence sorts before
  // it and belongs to this file; an older one sorts after it.
  Add("a", "k", 100, 50);
  const InternalKeyComparator icmp(BytewiseComparator());
  ASSERT_EQ(0u, FindFile(icmp, files_, InternalKey("k", 60, kTypeValue).Encode()));
  ASSERT_EQ(0u, FindFile(icmp, files_, InternalKey("k", 50, kTypeValue).Encode()));
  ASSERT_EQ(1u, FindFile(icmp, files_, InternalKey("k", 40, kTypeValue).Encode()));
}

// --- VersionSet and the manifest ---

class VersionSetTest : public testing::Test {
 protected:
  VersionSetTest() : dbname_(test::NewTestDir("version_set_test")) {
    table_options_.comparator = &icmp_;
    table_cache_ = std::make_unique<TableCache>(dbname_, table_options_, env_, 100);
  }

  std::unique_ptr<VersionSet> NewVersionSet() {
    return std::make_unique<VersionSet>(dbname_, env_, table_cache_.get(), &icmp_);
  }

  Status Apply(VersionSet* vset, VersionEdit* edit) {
    std::unique_lock lock(mutex_);
    return vset->LogAndApply(edit, &lock);
  }

  static void AddFile(VersionEdit* edit, int level, uint64_t number, const char* smallest,
                      const char* largest) {
    edit->AddFile(level, number, 1000 + number, InternalKey(smallest, 100, kTypeValue),
                  InternalKey(largest, 50, kTypeValue));
  }

  // Creates the database and applies a few edits that leave files at
  // several levels. Returns the resulting level layout.
  std::string BuildSampleState() {
    EXPECT_OK(VersionSet::CreateNew(dbname_, env_, BytewiseComparator()));
    auto vset = NewVersionSet();
    EXPECT_OK(vset->Recover());

    VersionEdit e1;
    AddFile(&e1, 0, vset->NewFileNumber(), "a", "m");
    AddFile(&e1, 0, vset->NewFileNumber(), "c", "z");
    AddFile(&e1, 1, vset->NewFileNumber(), "a", "f");
    AddFile(&e1, 1, vset->NewFileNumber(), "g", "p");
    AddFile(&e1, 3, vset->NewFileNumber(), "b", "y");
    e1.SetCompactPointer(1, InternalKey("f", 50, kTypeValue));
    EXPECT_OK(Apply(vset.get(), &e1));

    // Replace the two level-1 files with three, as a compaction would.
    VersionEdit e2;
    e2.RemoveFile(1, 5);
    e2.RemoveFile(1, 6);
    AddFile(&e2, 1, vset->NewFileNumber(), "a", "c");
    AddFile(&e2, 1, vset->NewFileNumber(), "d", "k");
    AddFile(&e2, 1, vset->NewFileNumber(), "l", "p");
    vset->SetLastSequence(12345);
    EXPECT_OK(Apply(vset.get(), &e2));

    EXPECT_OK(vset->current()->CheckInvariants());
    EXPECT_EQ(2, vset->NumLevelFiles(0));
    EXPECT_EQ(3, vset->NumLevelFiles(1));
    EXPECT_EQ(0, vset->NumLevelFiles(2));
    EXPECT_EQ(1, vset->NumLevelFiles(3));
    return vset->current()->DebugString();
  }

  std::string CurrentManifestPath() {
    std::string current;
    EXPECT_OK(ReadFileToString(env_, CurrentFileName(dbname_), &current));
    current.pop_back();
    return dbname_ + "/" + current;
  }

  // Replaces the live manifest with one holding exactly these edits.
  void WriteManifest(const std::vector<VersionEdit>& edits) {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(env_->NewWritableFile(CurrentManifestPath(), &file));
    log::Writer writer(file.get());
    for (const VersionEdit& edit : edits) {
      std::string record;
      edit.EncodeTo(&record);
      ASSERT_OK(writer.AddRecord(record));
    }
    ASSERT_OK(file->Close());
  }

  static VersionEdit BaseEdit() {
    VersionEdit edit;
    edit.SetComparatorName(BytewiseComparator()->Name());
    edit.SetLogNumber(0);
    edit.SetNextFile(100);
    edit.SetLastSequence(0);
    return edit;
  }

  Env* env_ = Env::Default();
  std::string dbname_;
  InternalKeyComparator icmp_{BytewiseComparator()};
  TableOptions table_options_;
  std::unique_ptr<TableCache> table_cache_;
  std::mutex mutex_;
};

TEST_F(VersionSetTest, NewDatabaseIsEmpty) {
  ASSERT_OK(VersionSet::CreateNew(dbname_, env_, BytewiseComparator()));
  ASSERT_TRUE(env_->FileExists(CurrentFileName(dbname_)));
  auto vset = NewVersionSet();
  ASSERT_OK(vset->Recover());
  for (int level = 0; level < config::kNumLevels; level++) {
    ASSERT_EQ(0, vset->NumLevelFiles(level));
  }
  ASSERT_EQ(0u, vset->LastSequence());
  ASSERT_EQ(0u, vset->LogNumber());
}

TEST_F(VersionSetTest, RecoverReconstructsTheSameLevels) {
  const std::string before = BuildSampleState();

  auto vset = NewVersionSet();
  ASSERT_OK(vset->Recover());
  ASSERT_EQ(before, vset->current()->DebugString());
  ASSERT_OK(vset->current()->CheckInvariants());
  ASSERT_EQ(12345u, vset->LastSequence());

  // File numbers continue past everything already handed out.
  ASSERT_GT(vset->NewFileNumber(), 10u);

  // And again after the recovered set writes its own manifest.
  VersionEdit edit;
  AddFile(&edit, 2, vset->NewFileNumber(), "q", "r");
  ASSERT_OK(Apply(vset.get(), &edit));
  const std::string after = vset->current()->DebugString();
  vset.reset();

  auto reopened = NewVersionSet();
  ASSERT_OK(reopened->Recover());
  ASSERT_EQ(after, reopened->current()->DebugString());
}

TEST_F(VersionSetTest, LevelZeroIsNewestFirstAndDeeperLevelsAreSorted) {
  BuildSampleState();
  auto vset = NewVersionSet();
  ASSERT_OK(vset->Recover());

  const auto& l0 = vset->current()->files(0);
  ASSERT_EQ(2u, l0.size());
  ASSERT_GT(l0[0]->number, l0[1]->number);

  const auto& l1 = vset->current()->files(1);
  ASSERT_EQ(3u, l1.size());
  ASSERT_EQ("a", l1[0]->smallest.user_key());
  ASSERT_EQ("d", l1[1]->smallest.user_key());
  ASSERT_EQ("l", l1[2]->smallest.user_key());
}

TEST_F(VersionSetTest, EachOpenStartsANewManifestAndSwitchesCurrent) {
  BuildSampleState();
  const std::string first_manifest = CurrentManifestPath();

  auto vset = NewVersionSet();
  ASSERT_OK(vset->Recover());
  // Nothing changes until the first edit is applied.
  ASSERT_EQ(first_manifest, CurrentManifestPath());

  VersionEdit edit;
  ASSERT_OK(Apply(vset.get(), &edit));
  const std::string second_manifest = CurrentManifestPath();
  ASSERT_NE(first_manifest, second_manifest);
  ASSERT_TRUE(env_->FileExists(second_manifest));
  // No temp file is left behind by the switch.
  for (const auto& entry : std::filesystem::directory_iterator(dbname_)) {
    ASSERT_NE(".dbtmp", entry.path().extension()) << entry.path();
  }
}

TEST_F(VersionSetTest, LiveFilesCoverEveryReferencedVersion) {
  ASSERT_OK(VersionSet::CreateNew(dbname_, env_, BytewiseComparator()));
  auto vset = NewVersionSet();
  ASSERT_OK(vset->Recover());

  VersionEdit add;
  AddFile(&add, 1, 50, "a", "b");
  ASSERT_OK(Apply(vset.get(), &add));

  // A reader pins the version that has file 50.
  Version* pinned = vset->current();
  pinned->Ref();

  VersionEdit replace;
  replace.RemoveFile(1, 50);
  AddFile(&replace, 1, 51, "a", "b");
  ASSERT_OK(Apply(vset.get(), &replace));

  std::set<uint64_t> live;
  vset->AddLiveFiles(&live);
  ASSERT_EQ((std::set<uint64_t>{50, 51}), live);  // 50 is still needed by the reader

  pinned->Unref();
  live.clear();
  vset->AddLiveFiles(&live);
  ASSERT_EQ((std::set<uint64_t>{51}), live);
}

TEST_F(VersionSetTest, TruncatedManifestTailIsIgnored) {
  const std::string before = BuildSampleState();
  const std::string manifest = CurrentManifestPath();

  // An edit that was being appended when the process died.
  {
    std::ofstream out(manifest, std::ios::binary | std::ios::app);
    out << std::string("\x01\x02\x03\x04\x40\x00\x01partial", 14);
  }
  auto vset = NewVersionSet();
  ASSERT_OK(vset->Recover());
  ASSERT_EQ(before, vset->current()->DebugString());
}

TEST_F(VersionSetTest, DamagedManifestFailsRecovery) {
  BuildSampleState();
  const std::string manifest = CurrentManifestPath();
  std::string contents;
  ASSERT_OK(ReadFileToString(env_, manifest, &contents));

  // Any flipped bit must fail recovery: the manifest is all checksummed
  // records.
  for (size_t byte = 0; byte < contents.size(); byte += 3) {
    std::string damaged = contents;
    damaged[byte] ^= 0x10;
    std::ofstream(manifest, std::ios::binary | std::ios::trunc) << damaged;
    auto vset = NewVersionSet();
    Status s = vset->Recover();
    ASSERT_TRUE(s.IsCorruption()) << "byte " << byte << ": " << s.ToString();
  }
}

TEST_F(VersionSetTest, BadCurrentFailsRecovery) {
  BuildSampleState();
  const std::string current = CurrentFileName(dbname_);
  std::string original;
  ASSERT_OK(ReadFileToString(env_, current, &original));

  std::ofstream(current, std::ios::trunc) << "MANIFEST-000001";  // no newline
  ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());

  std::ofstream(current, std::ios::trunc) << "";
  ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());

  std::ofstream(current, std::ios::trunc) << "MANIFEST-999999\n";  // does not exist
  ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());

  std::ofstream(current, std::ios::trunc) << original;
  ASSERT_OK(NewVersionSet()->Recover());
}

TEST_F(VersionSetTest, ComparatorMismatchIsRefused) {
  class ReverseComparator : public Comparator {
   public:
    int Compare(Slice a, Slice b) const override { return b.compare(a); }
    const char* Name() const override { return "test.ReverseComparator"; }
    void FindShortestSeparator(std::string*, Slice) const override {}
    void FindShortSuccessor(std::string*) const override {}
  } reverse;

  BuildSampleState();
  const InternalKeyComparator reverse_icmp(&reverse);
  VersionSet vset(dbname_, env_, table_cache_.get(), &reverse_icmp);
  Status s = vset.Recover();
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(VersionSetTest, ManifestMissingRequiredFieldsFailsRecovery) {
  ASSERT_OK(VersionSet::CreateNew(dbname_, env_, BytewiseComparator()));
  VersionEdit only_comparator;
  only_comparator.SetComparatorName(BytewiseComparator()->Name());
  WriteManifest({only_comparator});
  ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());
}

// A manifest whose records all pass their checksums can still describe a
// level structure the engine could never have produced.
TEST_F(VersionSetTest, RecoverRejectsVersionsThatBreakTheLevelInvariants) {
  ASSERT_OK(VersionSet::CreateNew(dbname_, env_, BytewiseComparator()));

  {
    VersionEdit overlapping = BaseEdit();
    AddFile(&overlapping, 1, 10, "a", "m");
    AddFile(&overlapping, 1, 11, "k", "z");
    WriteManifest({overlapping});
    Status s = NewVersionSet()->Recover();
    ASSERT_TRUE(s.IsCorruption()) << s.ToString();
    ASSERT_NE(std::string::npos, s.ToString().find("overlaps")) << s.ToString();
  }
  {
    // Adjacent files that share one user key also count as overlapping.
    VersionEdit shared_boundary = BaseEdit();
    AddFile(&shared_boundary, 2, 10, "a", "k");
    AddFile(&shared_boundary, 2, 11, "k", "z");
    WriteManifest({shared_boundary});
    ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());
  }
  {
    VersionEdit inverted = BaseEdit();
    AddFile(&inverted, 1, 10, "z", "a");
    WriteManifest({inverted});
    ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());
  }
  {
    VersionEdit duplicate = BaseEdit();
    AddFile(&duplicate, 1, 10, "a", "b");
    AddFile(&duplicate, 2, 10, "a", "b");
    WriteManifest({duplicate});
    ASSERT_TRUE(NewVersionSet()->Recover().IsCorruption());
  }
  {
    // Overlap in level 0 is normal.
    VersionEdit level0 = BaseEdit();
    AddFile(&level0, 0, 10, "a", "m");
    AddFile(&level0, 0, 11, "k", "z");
    WriteManifest({level0});
    ASSERT_OK(NewVersionSet()->Recover());
  }
}

}  // namespace lsm
