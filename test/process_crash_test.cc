#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <random>
#include <string>

#include "gtest/gtest.h"
#include "kill_child_keys.h"
#include "lsm/db.h"
#include "testutil.h"

namespace lsm {

namespace {

// Runs lsm_kill_child against dir, kills it with SIGKILL once it has
// acknowledged at least kill_after writes, and returns how many writes it
// acknowledged in total before dying.
uint32_t RunChildUntilKilled(const std::string& dir, bool sync, uint32_t kill_after) {
  int fds[2];
  EXPECT_EQ(0, ::pipe(fds));
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    ::execl(LSM_KILL_CHILD_PATH, LSM_KILL_CHILD_PATH, dir.c_str(), sync ? "1" : "0",
            static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(fds[1]);

  uint32_t acked = 0;
  uint32_t index;
  bool killed = false;
  // After the kill the pipe drains and then reports end of file; anything
  // read here was acknowledged before the process died.
  while (::read(fds[0], &index, sizeof(index)) == sizeof(index)) {
    acked = index + 1;
    if (!killed && acked >= kill_after) {
      ::kill(pid, SIGKILL);
      killed = true;
    }
  }
  ::close(fds[0]);

  int status = 0;
  EXPECT_EQ(pid, ::waitpid(pid, &status, 0));
  EXPECT_TRUE(killed) << "child exited on its own";
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "status " << status;
  return acked;
}

}  // namespace

// The sync=false contract: an acknowledged write has reached the OS, so
// killing the process cannot lose it. (Power loss can; see crash_test.)
TEST(ProcessCrash, AcknowledgedWritesSurviveSigkill) {
  std::mt19937 rnd(42);
  for (int round = 0; round < 6; round++) {
    const bool sync = round % 3 == 2;  // mostly the interesting case, sync=false
    const std::string dir = test::NewTestDir("process_crash");
    const uint32_t kill_after = sync ? 20 + rnd() % 50 : 200 + rnd() % 3000;
    const uint32_t acked = RunChildUntilKilled(dir, sync, kill_after);
    ASSERT_GE(acked, kill_after);

    DB* raw = nullptr;
    ASSERT_OK(DB::Open(Options(), dir, &raw));
    std::unique_ptr<DB> db(raw);

    // What survived must be a prefix of the writes, and at least as long as
    // what was acknowledged. A write can be durable but unacknowledged if
    // the kill landed between the two.
    uint32_t recovered = 0;
    std::string value;
    while (db->Get(ReadOptions(), test::KillChildKey(recovered), &value).ok()) {
      ASSERT_EQ(test::KillChildValue(recovered), value) << "key " << recovered;
      recovered++;
    }
    ASSERT_GE(recovered, acked) << "round " << round << " lost acknowledged writes";
    for (uint32_t i = recovered; i < recovered + 100; i++) {
      ASSERT_TRUE(db->Get(ReadOptions(), test::KillChildKey(i), &value).IsNotFound())
          << "hole before key " << i;
    }
  }
}

}  // namespace lsm
