// Feeds arbitrary bytes to the manifest record decoder. It must reject or
// accept without crashing, and anything it accepts must survive a round
// trip: encoding the decoded edit and decoding that again gives the same
// encoding.

#include <cstdint>
#include <string>

#include "db/version_edit.h"
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lsm;

  VersionEdit edit;
  if (!edit.DecodeFrom(Slice(reinterpret_cast<const char*>(data), size)).ok()) return 0;

  std::string encoded;
  edit.EncodeTo(&encoded);
  VersionEdit reparsed;
  FUZZ_CHECK(reparsed.DecodeFrom(encoded).ok());
  std::string reencoded;
  reparsed.EncodeTo(&reencoded);
  FUZZ_CHECK(encoded == reencoded);
  (void)edit.DebugString();
  return 0;
}
