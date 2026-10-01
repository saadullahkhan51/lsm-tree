// Microbenchmarks of the primitives the read and write paths spend their
// time in. Run build/release/micro_bench.

#include <benchmark/benchmark.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "lsm/comparator.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/cache.h"
#include "util/crc32c.h"

namespace lsm {
namespace {

std::string Key(uint64_t n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(n));
  return buf;
}

// One 4 KiB block, the unit every table read checksums.
void BM_Crc32c(benchmark::State& state) {
  const std::string block(4096, 'x');
  for (auto _ : state) benchmark::DoNotOptimize(crc32c::Value(block.data(), block.size()));
  state.SetBytesProcessed(state.iterations() * block.size());
}
BENCHMARK(BM_Crc32c);

void BM_Crc32cPortable(benchmark::State& state) {
  const std::string block(4096, 'x');
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c::internal::ExtendPortable(0, block.data(), block.size()));
  }
  state.SetBytesProcessed(state.iterations() * block.size());
}
BENCHMARK(BM_Crc32cPortable);

constexpr int kEntries = 100000;

MemTable* FilledMemTable(const InternalKeyComparator& comparator) {
  MemTable* mem = new MemTable(comparator);
  mem->Ref();
  std::mt19937_64 rnd(301);
  const std::string value(100, 'v');
  for (int i = 0; i < kEntries; i++) mem->Add(i + 1, kTypeValue, Key(rnd() % kEntries), value);
  return mem;
}

void BM_MemTableInsert(benchmark::State& state) {
  const InternalKeyComparator comparator(BytewiseComparator());
  for (auto _ : state) FilledMemTable(comparator)->Unref();
  state.SetItemsProcessed(state.iterations() * kEntries);
}
BENCHMARK(BM_MemTableInsert)->Unit(benchmark::kMillisecond);

void BM_MemTableGet(benchmark::State& state) {
  const InternalKeyComparator comparator(BytewiseComparator());
  MemTable* mem = FilledMemTable(comparator);
  std::mt19937_64 rnd(7);
  std::string value;
  Status s;
  for (auto _ : state) {
    const std::string key = Key(rnd() % kEntries);
    benchmark::DoNotOptimize(mem->Get(LookupKey(key, kMaxSequenceNumber), &value, &s));
  }
  mem->Unref();
}
BENCHMARK(BM_MemTableGet);

// A seek inside one data block: binary search over restart points, then a
// scan of up to 16 prefix-compressed entries.
void BM_BlockSeek(benchmark::State& state) {
  BlockBuilder builder(16);
  const std::string value(100, 'v');
  int entries = 0;
  while (builder.CurrentSizeEstimate() < 4096) builder.Add(Key(entries++), value);
  const Slice raw = builder.Finish();
  BlockContents contents;
  contents.data = std::make_unique<char[]>(raw.size());
  contents.size = raw.size();
  std::memcpy(contents.data.get(), raw.data(), raw.size());
  const Block block(std::move(contents));
  const std::unique_ptr<Iterator> iter(block.NewIterator(BytewiseComparator()));
  std::mt19937_64 rnd(7);
  for (auto _ : state) {
    iter->Seek(Key(rnd() % entries));
    benchmark::DoNotOptimize(iter->Valid());
  }
}
BENCHMARK(BM_BlockSeek);

// range(0) says whether the probed keys are in the filter. A missing key
// usually stops at the first unset bit; a present one pays for all k probes.
void BM_BloomProbe(benchmark::State& state) {
  FilterBlockBuilder builder(10);
  for (int i = 0; i < kEntries; i++) builder.AddKey(Key(i));
  const Slice filter = builder.Finish();
  const uint64_t offset = state.range(0) ? 0 : kEntries;
  std::mt19937_64 rnd(7);
  for (auto _ : state) {
    const std::string key = Key(offset + rnd() % kEntries);
    benchmark::DoNotOptimize(FilterKeyMayMatch(filter, key));
  }
}
BENCHMARK(BM_BloomProbe)->Arg(1)->Arg(0);

void BM_CacheLookup(benchmark::State& state) {
  ShardedLRUCache cache(1 << 20);
  for (int i = 0; i < 10000; i++) {
    cache.Release(cache.Insert(Key(i), nullptr, 1, [](Slice, void*) {}));
  }
  std::mt19937_64 rnd(7);
  for (auto _ : state) {
    const std::string key = Key(rnd() % 10000);
    ShardedLRUCache::Handle* handle = cache.Lookup(key);
    if (handle != nullptr) cache.Release(handle);
  }
}
BENCHMARK(BM_CacheLookup)->Threads(1)->Threads(8);

}  // namespace
}  // namespace lsm

BENCHMARK_MAIN();
