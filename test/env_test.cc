#include "lsm/env.h"

#include <algorithm>
#include <string>

#include "gtest/gtest.h"
#include "testutil.h"

namespace lsm {

class EnvTest : public testing::Test {
 protected:
  Status Write(const std::string& fname, Slice data) {
    std::unique_ptr<WritableFile> file;
    Status s = env_->NewWritableFile(fname, &file);
    if (s.ok()) s = file->Append(data);
    if (s.ok()) s = file->Close();
    return s;
  }

  std::string ReadAll(const std::string& fname) {
    std::unique_ptr<SequentialFile> file;
    EXPECT_OK(env_->NewSequentialFile(fname, &file));
    std::string contents;
    char scratch[8192];
    while (true) {
      Slice chunk;
      EXPECT_OK(file->Read(sizeof(scratch), &chunk, scratch));
      if (chunk.empty()) break;
      contents.append(chunk);
    }
    return contents;
  }

  Env* env_ = Env::Default();
  std::string dir_ = test::NewTestDir("env_test");
};

TEST_F(EnvTest, WriteThenReadSequential) {
  const std::string fname = dir_ + "/f";
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env_->NewWritableFile(fname, &file));
  ASSERT_OK(file->Append("hello "));
  ASSERT_OK(file->Append("world"));
  ASSERT_OK(file->Flush());
  ASSERT_OK(file->Sync());
  ASSERT_OK(file->Close());

  ASSERT_EQ("hello world", ReadAll(fname));

  uint64_t size;
  ASSERT_OK(env_->GetFileSize(fname, &size));
  ASSERT_EQ(11u, size);
}

TEST_F(EnvTest, AppendsLargerThanTheWriteBuffer) {
  // Crosses the 64 KiB buffer in every way Append handles: fill, spill
  // into the buffer, and bypass it entirely.
  const std::string fname = dir_ + "/big";
  std::string expected;
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env_->NewWritableFile(fname, &file));
  for (size_t n : {size_t{1}, size_t{65535}, size_t{10}, size_t{65536}, size_t{200000}, size_t{7}}) {
    const std::string chunk(n, static_cast<char>('a' + expected.size() % 26));
    ASSERT_OK(file->Append(chunk));
    expected += chunk;
  }
  ASSERT_OK(file->Close());
  ASSERT_EQ(expected, ReadAll(fname));
}

TEST_F(EnvTest, FlushMakesDataVisibleToReaders) {
  const std::string fname = dir_ + "/f";
  std::unique_ptr<WritableFile> file;
  ASSERT_OK(env_->NewWritableFile(fname, &file));
  ASSERT_OK(file->Append("abc"));
  ASSERT_OK(file->Flush());
  ASSERT_EQ("abc", ReadAll(fname));
  ASSERT_OK(file->Close());
}

TEST_F(EnvTest, NewWritableFileTruncates) {
  const std::string fname = dir_ + "/f";
  ASSERT_OK(Write(fname, "a long first version"));
  ASSERT_OK(Write(fname, "short"));
  ASSERT_EQ("short", ReadAll(fname));
}

TEST_F(EnvTest, SequentialSkip) {
  const std::string fname = dir_ + "/f";
  ASSERT_OK(Write(fname, "0123456789"));
  std::unique_ptr<SequentialFile> file;
  ASSERT_OK(env_->NewSequentialFile(fname, &file));
  char scratch[16];
  Slice result;
  ASSERT_OK(file->Skip(4));
  ASSERT_OK(file->Read(3, &result, scratch));
  ASSERT_EQ("456", result);
  ASSERT_OK(file->Read(16, &result, scratch));
  ASSERT_EQ("789", result);  // short read at end of file
  ASSERT_OK(file->Read(16, &result, scratch));
  ASSERT_TRUE(result.empty());
}

TEST_F(EnvTest, RandomAccessRead) {
  const std::string fname = dir_ + "/f";
  ASSERT_OK(Write(fname, "0123456789"));
  std::unique_ptr<RandomAccessFile> file;
  ASSERT_OK(env_->NewRandomAccessFile(fname, &file));
  char scratch[16];
  Slice result;
  ASSERT_OK(file->Read(3, 4, &result, scratch));
  ASSERT_EQ("3456", result);
  ASSERT_OK(file->Read(0, 2, &result, scratch));
  ASSERT_EQ("01", result);
  ASSERT_OK(file->Read(8, 10, &result, scratch));
  ASSERT_EQ("89", result);  // truncated at end of file
  ASSERT_OK(file->Read(100, 10, &result, scratch));
  ASSERT_TRUE(result.empty());
}

TEST_F(EnvTest, MissingFilesAreNotFound) {
  const std::string fname = dir_ + "/missing";
  std::unique_ptr<SequentialFile> seq;
  std::unique_ptr<RandomAccessFile> rand;
  uint64_t size;
  ASSERT_TRUE(env_->NewSequentialFile(fname, &seq).IsNotFound());
  ASSERT_TRUE(env_->NewRandomAccessFile(fname, &rand).IsNotFound());
  ASSERT_TRUE(env_->GetFileSize(fname, &size).IsNotFound());
  ASSERT_TRUE(env_->RemoveFile(fname).IsNotFound());
  ASSERT_FALSE(env_->FileExists(fname));
}

TEST_F(EnvTest, DirectoryOperations) {
  const std::string sub = dir_ + "/sub";
  ASSERT_OK(env_->CreateDir(sub));
  ASSERT_OK(env_->CreateDir(sub));  // already exists: still OK

  ASSERT_OK(Write(sub + "/a", "1"));
  ASSERT_OK(Write(sub + "/b", "2"));
  ASSERT_TRUE(env_->FileExists(sub + "/a"));

  std::vector<std::string> children;
  ASSERT_OK(env_->GetChildren(sub, &children));
  std::sort(children.begin(), children.end());
  ASSERT_EQ((std::vector<std::string>{"a", "b"}), children);

  ASSERT_OK(env_->RenameFile(sub + "/a", sub + "/c"));
  ASSERT_FALSE(env_->FileExists(sub + "/a"));
  ASSERT_EQ("1", ReadAll(sub + "/c"));

  // Rename replaces an existing target atomically.
  ASSERT_OK(env_->RenameFile(sub + "/c", sub + "/b"));
  ASSERT_EQ("1", ReadAll(sub + "/b"));

  ASSERT_OK(env_->SyncDir(sub));

  ASSERT_FALSE(env_->RemoveDir(sub).ok());  // not empty
  ASSERT_OK(env_->RemoveFile(sub + "/b"));
  ASSERT_OK(env_->RemoveDir(sub));
  ASSERT_TRUE(env_->GetChildren(sub, &children).IsNotFound());
  ASSERT_TRUE(env_->SyncDir(sub).IsNotFound());
}

TEST_F(EnvTest, LockIsExclusive) {
  const std::string fname = dir_ + "/LOCK";
  FileLock* lock = nullptr;
  ASSERT_OK(env_->LockFile(fname, &lock));
  ASSERT_NE(nullptr, lock);

  FileLock* second = nullptr;
  ASSERT_TRUE(env_->LockFile(fname, &second).IsIOError());
  ASSERT_EQ(nullptr, second);

  ASSERT_OK(env_->UnlockFile(lock));
  ASSERT_OK(env_->LockFile(fname, &second));
  ASSERT_OK(env_->UnlockFile(second));
}

}  // namespace lsm
