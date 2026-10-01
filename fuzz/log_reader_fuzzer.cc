// Fuzzes the log reader two ways, chosen by the first input byte.
//
// Raw: the rest of the input is read as a log file. The reader must not
// crash, whatever it is given.
//
// Damaged: the input describes a set of records and one piece of damage. A
// valid log is written from the records, the damage is applied, and the
// result is read back. What the reader returns must then be a prefix of
// what was written, and:
//
//   - undamaged: everything, with no error;
//   - cut short: exactly the records that fit, with no error, because a
//     short log is what a crash leaves and must always be recoverable;
//   - one bit flipped: an error, unless the reader still returned
//     everything (the bit was in block padding);
//   - tail zeroed: a prefix, with or without an error.
//
// In no case may the reader return a record that differs from the one
// written, or skip one and return a later one.

#include <cstdint>
#include <string>
#include <vector>

#include "db/log_reader.h"
#include "db/log_writer.h"
#include "fuzz_util.h"

namespace {

using namespace lsm;

struct ReadResult {
  std::vector<std::string> records;
  Status status;
};

ReadResult ReadAll(const std::string& file) {
  fuzz::StringSequentialFile source(file);
  log::Reader reader(&source);
  ReadResult result;
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) result.records.emplace_back(record);
  result.status = reader.status();
  return result;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  fuzz::Input in(data, size);
  if (in.Byte() % 2 == 0) {
    (void)ReadAll(in.Rest());
    return 0;
  }

  // Up to 8 records. Sizes are biased so that some are empty, most are
  // small, and some span one or more 32 KiB blocks.
  fuzz::StringSink sink;
  log::Writer writer(&sink);
  std::vector<std::string> written;
  std::vector<size_t> ends;  // file size after each record
  const int count = in.Byte() % 9;
  for (int i = 0; i < count; i++) {
    const uint32_t selector = in.Byte();
    size_t length = in.U16();
    if (selector % 4 == 0) {
      length %= 8;
    } else if (selector % 4 != 3) {
      length %= 600;
    } else {
      length += 20000 * (selector % 5);
    }
    std::string record(length, static_cast<char>('a' + i));
    for (size_t j = 0; j < record.size(); j += 97) record[j] = static_cast<char>(in.Byte());
    FUZZ_CHECK(writer.AddRecord(record).ok());
    written.push_back(std::move(record));
    ends.push_back(sink.contents.size());
  }

  const fuzz::Mutation mutation = fuzz::ReadMutation(&in, sink.contents.size());
  const ReadResult result = ReadAll(fuzz::Apply(mutation, sink.contents));

  // Always: a prefix of what was written, and an error only as Corruption.
  FUZZ_CHECK(result.records.size() <= written.size());
  for (size_t i = 0; i < result.records.size(); i++) {
    FUZZ_CHECK(result.records[i] == written[i]);
  }
  FUZZ_CHECK(result.status.ok() || result.status.IsCorruption());

  if (sink.contents.empty()) return 0;
  switch (mutation.kind) {
    case 0:
      FUZZ_CHECK(result.status.ok());
      FUZZ_CHECK(result.records.size() == written.size());
      break;
    case 1: {
      FUZZ_CHECK(result.status.ok());
      size_t complete = 0;
      while (complete < ends.size() && ends[complete] <= mutation.offset) complete++;
      FUZZ_CHECK(result.records.size() == complete);
      break;
    }
    case 2:
      FUZZ_CHECK(!result.status.ok() || result.records.size() == written.size());
      break;
    default:
      break;
  }
  return 0;
}
