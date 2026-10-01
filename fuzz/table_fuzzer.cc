// Fuzzes the table reader two ways, chosen by the first input byte.
//
// Raw: the rest of the input is opened as a table file. This reaches the
// footer parser and the block-handle checks; little gets past the block
// checksums, which is the point of them.
//
// Damaged: the input describes a set of entries and one piece of damage. A
// valid table is built, the damage is applied, and the result is opened
// and read in full. Either some step reports an error, or the contents
// read back are exactly the entries written. Different contents reported
// as success is the failure being hunted.

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "db/dbformat.h"
#include "fuzz_util.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace {

using namespace lsm;

const InternalKeyComparator kComparator(BytewiseComparator());

struct InternalLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return kComparator.Compare(a, b) < 0;
  }
};
using Entries = std::map<std::string, std::string, InternalLess>;

// Opens file as a table and reads it all. Returns false if any step
// reported an error.
bool ReadTable(const TableOptions& options, const std::string& file, Entries* out) {
  std::unique_ptr<Table> table;
  if (!Table::Open(options, std::make_unique<fuzz::StringRandomAccessFile>(file), 1, file.size(),
                   &table)
           .ok()) {
    return false;
  }
  std::unique_ptr<Iterator> iter(table->NewIterator(ReadOptions()));
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    out->emplace(iter->key(), iter->value());
  }
  if (!iter->status().ok()) return false;

  // The same through point lookups, which use the filter and one block.
  for (const auto& [key, value] : *out) {
    struct Found {
      bool called = false;
      std::string key, value;
    } found;
    if (!table
             ->InternalGet(ReadOptions(), key, &found,
                           [](void* arg, Slice k, Slice v) {
                             auto* f = static_cast<Found*>(arg);
                             f->called = true;
                             f->key.assign(k);
                             f->value.assign(v);
                           })
             .ok()) {
      return false;
    }
    FUZZ_CHECK(found.called && found.key == key && found.value == value);
  }
  return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  fuzz::Input in(data, size);
  TableOptions options;
  options.comparator = &kComparator;

  if (in.Byte() % 2 == 0) {
    Entries ignored;
    (void)ReadTable(options, in.Rest(), &ignored);
    return 0;
  }

  options.block_size = 16 + in.Byte();  // small blocks: several per table
  options.block_restart_interval = 1 + in.Byte() % 16;
  options.bloom_bits_per_key = in.Byte() % 2 == 0 ? 10 : 0;

  Entries entries;
  const int count = in.Byte() % 40;
  for (int i = 0; i < count; i++) {
    std::string internal_key;
    AppendInternalKey(&internal_key,
                      ParsedInternalKey{in.Bytes(in.Byte() % 6), in.Byte(),
                                        in.Byte() % 2 == 0 ? kTypeValue : kTypeDeletion});
    entries[internal_key] = in.Bytes(in.Byte() % 40);
  }

  fuzz::StringSink sink;
  {
    TableBuilder builder(options, &sink);
    for (const auto& [key, value] : entries) builder.Add(key, value);
    FUZZ_CHECK(builder.Finish().ok());
  }

  const fuzz::Mutation mutation = fuzz::ReadMutation(&in, sink.contents.size());
  Entries read_back;
  const bool ok = ReadTable(options, fuzz::Apply(mutation, sink.contents), &read_back);
  if (mutation.kind == 0) FUZZ_CHECK(ok);
  if (ok) FUZZ_CHECK(read_back == entries);
  return 0;
}
