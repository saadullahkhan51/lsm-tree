// Writes to a database until killed. After each write is acknowledged, its
// index goes to stdout as a raw uint32. process_crash_test reads those,
// sends SIGKILL at a moment of its choosing, and checks that every
// acknowledged write is still there.
//
// usage: lsm_kill_child <db dir> <sync: 0|1>

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "kill_child_keys.h"
#include "lsm/db.h"

int main(int argc, char** argv) {
  if (argc != 3) return 64;
  lsm::DB* db = nullptr;
  if (!lsm::DB::Open(lsm::Options(), argv[1], &db).ok()) return 2;

  lsm::WriteOptions write_options;
  write_options.sync = std::atoi(argv[2]) != 0;
  for (uint32_t i = 0;; i++) {
    if (!db->Put(write_options, lsm::test::KillChildKey(i), lsm::test::KillChildValue(i)).ok()) {
      return 3;
    }
    // Only now is the write acknowledged.
    if (::write(STDOUT_FILENO, &i, sizeof(i)) != sizeof(i)) return 4;
  }
}
