#include "db/version_set.h"

#include <algorithm>
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
    return std::make_unique<VersionSet>(dbname_, &options_, table_cache_.get(), &icmp_);
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
  Options options_;
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
  VersionSet vset(dbname_, &options_, table_cache_.get(), &reverse_icmp);
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

// --- Choosing compactions ---

class CompactionPickerTest : public VersionSetTest {
 protected:
  CompactionPickerTest() {
    // Small targets so that a handful of fake files crosses them.
    options_.max_file_size = 1000;
    options_.max_bytes_for_level_base = 10000;
    options_.level_size_multiplier = 10;
    EXPECT_OK(VersionSet::CreateNew(dbname_, env_, BytewiseComparator()));
    vset_ = NewVersionSet();
    EXPECT_OK(vset_->Recover());
  }

  // Adds a file of the given size covering user keys [smallest, largest].
  uint64_t Add(int level, const char* smallest, const char* largest, uint64_t size = 1000) {
    const uint64_t number = vset_->NewFileNumber();
    VersionEdit edit;
    edit.AddFile(level, number, size, InternalKey(smallest, 100, kTypeValue),
                 InternalKey(largest, 50, kTypeValue));
    EXPECT_OK(Apply(vset_.get(), &edit));
    return number;
  }

  // The file numbers of one side of a compaction's inputs.
  static std::vector<uint64_t> Inputs(const Compaction& c, int which) {
    std::vector<uint64_t> numbers;
    for (int i = 0; i < c.num_input_files(which); i++) numbers.push_back(c.input(which, i)->number);
    std::sort(numbers.begin(), numbers.end());
    return numbers;
  }

  std::unique_ptr<Compaction> Pick() { return std::unique_ptr<Compaction>(vset_->PickCompaction()); }

  std::unique_ptr<VersionSet> vset_;
};

TEST_F(CompactionPickerTest, NothingToDoBelowTheTargets) {
  ASSERT_FALSE(vset_->NeedsCompaction());
  ASSERT_EQ(nullptr, Pick());

  Add(0, "a", "b");
  Add(0, "c", "d");
  Add(0, "e", "f");                 // 3 level-0 files: trigger is 4
  Add(1, "a", "m", /*size=*/9000);  // level 1 under its 10000 target
  ASSERT_FALSE(vset_->NeedsCompaction());
  ASSERT_EQ(nullptr, Pick());
}

TEST_F(CompactionPickerTest, LevelTargetsGrowByTheMultiplier) {
  ASSERT_EQ(10000u, vset_->MaxBytesForLevel(1));
  ASSERT_EQ(100000u, vset_->MaxBytesForLevel(2));
  ASSERT_EQ(1000000u, vset_->MaxBytesForLevel(3));
}

TEST_F(CompactionPickerTest, FourLevelZeroFilesTriggerACompaction) {
  const uint64_t f1 = Add(0, "a", "e");
  const uint64_t f2 = Add(0, "d", "k");
  const uint64_t f3 = Add(0, "x", "z");  // overlaps nothing
  const uint64_t f4 = Add(0, "j", "m");
  const uint64_t l1a = Add(1, "a", "c");
  const uint64_t l1b = Add(1, "f", "l");
  Add(1, "p", "q");  // outside the compaction's range
  ASSERT_TRUE(vset_->NeedsCompaction());

  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(0, c->level());
  // Every level-0 file that overlaps the chosen one, transitively: f1-f2-f4
  // chain together through shared ranges; f3 stands apart.
  ASSERT_EQ((std::vector<uint64_t>{f1, f2, f4}), Inputs(*c, 0));
  (void)f3;
  ASSERT_EQ((std::vector<uint64_t>{l1a, l1b}), Inputs(*c, 1));
  ASSERT_FALSE(c->IsTrivialMove());
}

TEST_F(CompactionPickerTest, TheLevelFurthestOverItsTargetGoesFirst) {
  for (const char* k : {"a", "c", "e", "g"}) Add(0, k, k);       // level 0 at 4/4 = 1.0
  Add(1, "a", "m", 30000);                                       // level 1 at 3.0
  Add(2, "a", "z", 150000);                                      // level 2 at 1.5
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(1, c->level());
}

TEST_F(CompactionPickerTest, SuccessiveCompactionsSweepTheLevel) {
  // Level 1 is over target with four files. Each pick takes the file
  // after the previous one, then wraps.
  const uint64_t f1 = Add(1, "a", "b", 4000);
  const uint64_t f2 = Add(1, "c", "d", 4000);
  const uint64_t f3 = Add(1, "e", "f", 4000);
  const uint64_t f4 = Add(1, "g", "h", 4000);

  std::vector<uint64_t> order;
  for (int i = 0; i < 5; i++) {
    auto c = Pick();
    ASSERT_NE(nullptr, c);
    ASSERT_EQ(1, c->level());
    ASSERT_EQ(1, c->num_input_files(0));
    order.push_back(c->input(0, 0)->number);
  }
  ASSERT_EQ((std::vector<uint64_t>{f1, f2, f3, f4, f1}), order);
}

TEST_F(CompactionPickerTest, TheCompactionPointerSurvivesARestart) {
  const uint64_t f1 = Add(1, "a", "b", 4000);
  const uint64_t f2 = Add(1, "c", "d", 4000);
  Add(1, "e", "f", 4000);

  {
    auto c = Pick();
    ASSERT_EQ(f1, c->input(0, 0)->number);
    // Installing the compaction's edit persists the pointer. (The edit
    // here changes no files, so f1 is still present afterwards.)
    ASSERT_OK(Apply(vset_.get(), c->edit()));
  }
  vset_ = NewVersionSet();
  ASSERT_OK(vset_->Recover());
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(f2, c->input(0, 0)->number);
}

TEST_F(CompactionPickerTest, LoneFileWithNothingBelowIsATrivialMove) {
  Add(1, "a", "b", 20000);
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(1, c->num_input_files(0));
  ASSERT_EQ(0, c->num_input_files(1));
  ASSERT_TRUE(c->IsTrivialMove());
}

TEST_F(CompactionPickerTest, NoTrivialMoveOntoTooMuchGrandparentData) {
  // Moving the file down would leave it overlapping more than
  // 10 x max_file_size of the level below its new home.
  Add(1, "a", "z", 20000);
  for (const char* k : {"b", "d", "f", "h", "j", "l"}) Add(3, k, k, 2000);
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(1, c->level());
  ASSERT_FALSE(c->IsTrivialMove());
}

TEST_F(CompactionPickerTest, InputsExpandWhenTheNextLevelIsRewrittenAnyway) {
  // The one level-2 file spans both level-1 files. Compacting either
  // rewrites it, so the other comes along for free.
  const uint64_t a = Add(1, "a", "c", 6000);
  const uint64_t b = Add(1, "d", "f", 6000);
  const uint64_t below = Add(2, "a", "f", 3000);
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ((std::vector<uint64_t>{a, b}), Inputs(*c, 0));
  ASSERT_EQ((std::vector<uint64_t>{below}), Inputs(*c, 1));
}

TEST_F(CompactionPickerTest, InputsDoNotExpandIfThatPullsInMoreOfTheNextLevel) {
  const uint64_t a = Add(1, "a", "c", 6000);
  Add(1, "d", "m", 6000);  // would drag in the second level-2 file
  const uint64_t below1 = Add(2, "a", "f", 3000);
  Add(2, "k", "p", 3000);
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ((std::vector<uint64_t>{a}), Inputs(*c, 0));
  ASSERT_EQ((std::vector<uint64_t>{below1}), Inputs(*c, 1));
}

TEST_F(CompactionPickerTest, OutputIsCutWhenItOverlapsTooMuchOfTheGrandparentLevel) {
  // Twelve 2000-byte files two levels down. The limit is 10 x 1000 bytes,
  // so an output file must be cut after passing six of them.
  Add(1, "a", "z", 20000);
  Add(2, "a", "z", 1000);
  const char* keys[] = {"b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m"};
  for (const char* k : keys) Add(3, k, k, 2000);

  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(1, c->level());

  int cuts = 0;
  std::string cut_at;
  for (char ch = 'a'; ch <= 'z'; ch++) {
    const InternalKey key(std::string(1, ch) + "x", 10, kTypeValue);
    if (c->ShouldStopBefore(key.Encode())) {
      cuts++;
      cut_at.push_back(ch);
    }
  }
  ASSERT_EQ(2, cuts) << cut_at;
}

TEST_F(CompactionPickerTest, BaseLevelMeansNoDeeperLevelCanHoldTheKey) {
  Add(1, "a", "z", 20000);
  Add(3, "d", "f");
  Add(4, "p", "q");
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_EQ(1, c->level());

  // Asked in increasing key order, as a compaction does.
  ASSERT_TRUE(c->IsBaseLevelForKey("a"));
  ASSERT_FALSE(c->IsBaseLevelForKey("d"));   // inside the level-3 file's range
  ASSERT_FALSE(c->IsBaseLevelForKey("e"));
  ASSERT_FALSE(c->IsBaseLevelForKey("f"));
  ASSERT_TRUE(c->IsBaseLevelForKey("g"));
  ASSERT_FALSE(c->IsBaseLevelForKey("p"));   // inside the level-4 file's range
  ASSERT_TRUE(c->IsBaseLevelForKey("r"));
}

TEST_F(CompactionPickerTest, BaseLevelIgnoresTheOutputLevelItself) {
  // The compaction's own output level holds the key, but that file is
  // one of the inputs being merged, so it is not "below".
  Add(1, "a", "z", 20000);
  Add(2, "a", "z");
  auto c = Pick();
  ASSERT_NE(nullptr, c);
  ASSERT_TRUE(c->IsBaseLevelForKey("m"));
}

TEST_F(CompactionPickerTest, ManualCompactionTakesTheFilesInRange) {
  const uint64_t a = Add(1, "a", "c", 500);
  const uint64_t b = Add(1, "d", "f", 300);
  const uint64_t d = Add(1, "g", "i", 300);
  Add(1, "x", "z", 300);

  const InternalKey begin("b", kMaxSequenceNumber, kValueTypeForSeek);
  const InternalKey end("h", 0, kTypeDeletion);
  std::unique_ptr<Compaction> c(vset_->CompactRange(1, &begin, &end));
  ASSERT_NE(nullptr, c);
  // Stops once it has a table's worth (1000 bytes): a, b, and d reach it.
  ASSERT_EQ((std::vector<uint64_t>{a, b, d}), Inputs(*c, 0));

  const InternalKey none_begin("j", kMaxSequenceNumber, kValueTypeForSeek);
  const InternalKey none_end("w", 0, kTypeDeletion);
  ASSERT_EQ(nullptr, vset_->CompactRange(1, &none_begin, &none_end));
  ASSERT_EQ(nullptr, vset_->CompactRange(2, nullptr, nullptr));
}

TEST_F(CompactionPickerTest, OverlapQueriesUseUserKeys) {
  Add(0, "c", "g");
  Add(0, "f", "k");  // chains with the first
  Add(0, "p", "q");
  Add(1, "c", "e");

  const Version* v = vset_->current();
  const Slice b = "b", c = "c", h = "h", l = "l", n = "n", z = "z";
  ASSERT_FALSE(v->OverlapInLevel(1, nullptr, &b));
  ASSERT_TRUE(v->OverlapInLevel(1, &b, &c));
  ASSERT_TRUE(v->OverlapInLevel(1, nullptr, nullptr));
  ASSERT_FALSE(v->OverlapInLevel(1, &h, nullptr));
  ASSERT_FALSE(v->OverlapInLevel(0, &l, &n));
  ASSERT_TRUE(v->OverlapInLevel(0, &n, &z));
  ASSERT_FALSE(v->OverlapInLevel(2, nullptr, nullptr));

  // In level 0, asking for [g, g] returns the first file, whose range then
  // widens the search to include the second.
  const InternalKey g("g", kMaxSequenceNumber, kValueTypeForSeek);
  std::vector<FileMetaData*> inputs;
  v->GetOverlappingInputs(0, &g, &g, &inputs);
  ASSERT_EQ(2u, inputs.size());
}

}  // namespace lsm
