// Feeds arbitrary bytes to the block decoder. A block that fails its
// checksum never reaches the decoder, so this is the second line of
// defense: whatever the bytes are, iterating them must not read out of
// bounds, loop forever, or crash. Run with ASan.

#include <cstdint>
#include <cstring>
#include <memory>

#include "fuzz_util.h"
#include "lsm/comparator.h"
#include "table/block.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lsm;

  BlockContents contents;
  contents.data = std::make_unique<char[]>(size);
  std::memcpy(contents.data.get(), data, size);
  contents.size = size;
  const Block block(std::move(contents));
  std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));

  // Touch every byte the iterator hands out, so ASan sees a bad slice.
  volatile size_t sink = 0;
  auto touch = [&] {
    for (char c : iter->key()) sink = sink + static_cast<uint8_t>(c);
    for (char c : iter->value()) sink = sink + static_cast<uint8_t>(c);
  };

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) touch();
  // Prev rescans from a restart point, so a full backward pass can be
  // quadratic in a block with few restart points. Bound it.
  int steps = 0;
  for (iter->SeekToLast(); iter->Valid() && steps < 64; iter->Prev(), steps++) touch();

  // Seek to a few targets cut from the input itself.
  for (size_t start = 0; start < size && start < 64; start += 7) {
    const Slice target(reinterpret_cast<const char*>(data) + start,
                       std::min<size_t>(size - start, 1 + start % 9));
    iter->Seek(target);
    if (iter->Valid()) {
      touch();
      iter->Next();
    }
    if (iter->Valid()) {
      touch();
      iter->Prev();
    }
    if (iter->Valid()) touch();
  }
  (void)iter->status();
  return 0;
}
