// db_bench: the standard LevelDB/RocksDB workloads plus a YCSB-style mix,
// run against this engine, LevelDB, or RocksDB with the same settings.
//
//   db_bench --engine=lsm --benchmarks=fillrandom,readrandom --num=1000000
//
// For each benchmark it reports throughput, p50/p99/p99.9 latency, and the
// bytes the process wrote and read while it ran. With --json=FILE it also
// appends one JSON object per benchmark, which the scripts in bench/ turn
// into tables and charts.
//
// Benchmarks:
//   fillseq        write N keys in order
//   fillrandom     write N keys in random order
//   overwrite      write N random keys into an existing database
//   fillsync       write N/1000 random keys, each synced
//   readrandom     read N random keys that exist
//   readmissing    read N keys that do not exist
//   readseq        scan forward
//   readreverse    scan backward
//   seekrandom     N seeks to random keys
//   readwhilewriting  N random reads per thread while one thread writes
//   ycsba..ycsbf   the YCSB core workloads (zipfian keys)
//   compact        compact the whole key range
//   waitforbg      wait for flushes and compactions to finish
//   stats          print the engine's statistics

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#endif

namespace bench {

namespace {

using Clock = std::chrono::steady_clock;

struct Flags {
  std::string engine = "lsm";
  std::string benchmarks = "fillseq,fillrandom,overwrite,readrandom,readseq";
  std::string db = "/tmp/lsm_bench";
  std::string json;
  // Recorded with each JSON result, to tell runs of one engine apart.
  std::string label;
  uint64_t num = 1000000;
  int64_t reads = -1;  // -1: same as num
  int threads = 1;
  int key_size = 16;
  int value_size = 100;
  bool sync = false;
  bool use_existing_db = false;
  uint32_t seed = 301;
  // How many points of on-disk size to record during a write benchmark.
  int space_samples = 0;
  // Pace each writing thread to this many writes a second. 0: no limit.
  double writes_per_sec = 0;
  EngineConfig config;
};

Flags FLAGS;

// --- Process I/O counters: what the process wrote and read, whichever
// engine did it.

struct IoCounters {
  uint64_t bytes_written = 0;  // bytes passed to write calls
  uint64_t bytes_read = 0;     // bytes read from the storage device
};

IoCounters ReadIoCounters() {
  IoCounters io;
#if defined(__APPLE__)
  rusage_info_v4 usage;
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) == 0) {
    io.bytes_written = usage.ri_logical_writes;
    io.bytes_read = usage.ri_diskio_bytesread;
  }
#else
  if (FILE* f = std::fopen("/proc/self/io", "r")) {
    char name[64];
    unsigned long long value;
    while (std::fscanf(f, "%63[^:]: %llu\n", name, &value) == 2) {
      if (std::strcmp(name, "wchar") == 0) io.bytes_written = value;
      if (std::strcmp(name, "read_bytes") == 0) io.bytes_read = value;
    }
    std::fclose(f);
  }
#endif
  return io;
}

uint64_t DirectorySize(const std::string& path) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    const uint64_t size = entry.file_size(ec);
    if (!ec) total += size;
  }
  return total;
}

// A fingerprint of the directory: the name and size of every file.
std::string DirectoryFingerprint(const std::string& path) {
  std::vector<std::string> entries;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    entries.push_back(entry.path().filename().string() + ":" + std::to_string(entry.file_size(ec)));
  }
  std::sort(entries.begin(), entries.end());
  std::string out;
  for (const std::string& e : entries) out += e + "\n";
  return out;
}

// Returns once nothing in the directory has changed for two seconds.
void WaitUntilDirectoryIsStable(const std::string& path) {
  std::string last = DirectoryFingerprint(path);
  int stable_polls = 0;
  while (stable_polls < 4) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string now = DirectoryFingerprint(path);
    stable_polls = now == last ? stable_polls + 1 : 0;
    last = std::move(now);
  }
}

// --- Keys and values.

std::string MakeKey(uint64_t n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%0*llu", FLAGS.key_size, static_cast<unsigned long long>(n));
  return buf;
}

// Values are slices of one buffer of random bytes. Compression is off in
// every engine, so their content does not matter beyond not being constant.
class ValueSource {
 public:
  explicit ValueSource(uint32_t seed) {
    std::mt19937 rnd(seed);
    data_.resize((1 << 20) + FLAGS.value_size);
    for (char& c : data_) c = static_cast<char>(' ' + rnd() % 95);
  }
  std::string_view Next() {
    if (pos_ + FLAGS.value_size > data_.size()) pos_ = 0;
    pos_ += FLAGS.value_size;
    return std::string_view(data_.data() + pos_ - FLAGS.value_size, FLAGS.value_size);
  }

 private:
  std::string data_;
  size_t pos_ = 0;
};

// Zipfian over [0, n), as in YCSB (Gray et al., "Quickly generating
// billion-record synthetic databases"), scrambled so that the popular items
// are spread over the key space rather than clustered at its start.
class Zipfian {
 public:
  Zipfian(uint64_t n, uint32_t seed) : n_(n), rnd_(seed) {
    for (uint64_t i = 1; i <= n_; i++) zetan_ += 1.0 / std::pow(static_cast<double>(i), kTheta);
    const double zeta2 = 1.0 + 1.0 / std::pow(2.0, kTheta);
    alpha_ = 1.0 / (1.0 - kTheta);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - kTheta)) / (1.0 - zeta2 / zetan_);
  }

  uint64_t Next() {
    const double u = std::uniform_real_distribution<double>(0, 1)(rnd_);
    const double uz = u * zetan_;
    uint64_t rank;
    if (uz < 1.0) {
      rank = 0;
    } else if (uz < 1.0 + std::pow(0.5, kTheta)) {
      rank = 1;
    } else {
      rank = static_cast<uint64_t>(static_cast<double>(n_) *
                                   std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }
    // FNV-1a scramble.
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < 8; i++) {
      h ^= (rank >> (8 * i)) & 0xff;
      h *= 1099511628211ull;
    }
    return h % n_;
  }

 private:
  static constexpr double kTheta = 0.99;
  const uint64_t n_;
  std::mt19937_64 rnd_;
  double zetan_ = 0;
  double alpha_;
  double eta_;
};

// --- Per-thread results.

struct ThreadStats {
  std::vector<uint32_t> latencies_ns;  // one per operation
  uint64_t ops = 0;
  uint64_t found = 0;
  uint64_t bytes = 0;  // key and value bytes written or read
  uint64_t errors = 0;

  void Record(Clock::time_point start) {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    latencies_ns.push_back(static_cast<uint32_t>(std::min<int64_t>(ns, UINT32_MAX)));
    ops++;
  }
};

struct ThreadArg {
  int index;
  uint64_t ops;  // how many operations this thread should do
  Engine* engine;
  ThreadStats stats;
  std::atomic<bool>* stop;  // for background threads
};

using BenchFn = void (*)(ThreadArg*);

// --- The benchmarks. Each is the body of one thread.

void DoWrite(ThreadArg* arg, bool sequential) {
  std::mt19937_64 rnd(FLAGS.seed + arg->index);
  ValueSource values(FLAGS.seed + arg->index);
  const auto begin = Clock::now();
  for (uint64_t i = 0; i < arg->ops; i++) {
    const std::string key = MakeKey(sequential ? i : rnd() % FLAGS.num);
    const std::string_view value = values.Next();
    if (FLAGS.writes_per_sec > 0) {
      std::this_thread::sleep_until(begin + std::chrono::duration<double>(i / FLAGS.writes_per_sec));
    }
    const auto start = Clock::now();
    if (!arg->engine->Put(key, value, FLAGS.sync)) arg->stats.errors++;
    arg->stats.Record(start);
    arg->stats.bytes += key.size() + value.size();
  }
}

void FillSeq(ThreadArg* arg) { DoWrite(arg, true); }
void FillRandom(ThreadArg* arg) { DoWrite(arg, false); }

void ReadRandom(ThreadArg* arg) {
  std::mt19937_64 rnd(FLAGS.seed + 1000 + arg->index);
  std::string value;
  for (uint64_t i = 0; i < arg->ops; i++) {
    const std::string key = MakeKey(rnd() % FLAGS.num);
    bool found = false;
    const auto start = Clock::now();
    if (!arg->engine->Get(key, &value, &found)) arg->stats.errors++;
    arg->stats.Record(start);
    if (found) {
      arg->stats.found++;
      arg->stats.bytes += key.size() + value.size();
    }
  }
}

void ReadMissing(ThreadArg* arg) {
  std::mt19937_64 rnd(FLAGS.seed + 2000 + arg->index);
  std::string value;
  for (uint64_t i = 0; i < arg->ops; i++) {
    // Same length and range as a real key, but never written.
    std::string key = MakeKey(rnd() % FLAGS.num);
    key.back() = '.';
    bool found = false;
    const auto start = Clock::now();
    if (!arg->engine->Get(key, &value, &found)) arg->stats.errors++;
    arg->stats.Record(start);
    if (found) arg->stats.found++;
  }
}

void Scan(ThreadArg* arg, bool forward) {
  std::unique_ptr<Cursor> cursor = arg->engine->NewCursor();
  auto start = Clock::now();
  if (forward) {
    cursor->SeekToFirst();
  } else {
    cursor->SeekToLast();
  }
  for (uint64_t i = 0; i < arg->ops && cursor->Valid(); i++) {
    arg->stats.bytes += cursor->key().size() + cursor->value().size();
    arg->stats.found++;
    if (forward) {
      cursor->Next();
    } else {
      cursor->Prev();
    }
    arg->stats.Record(start);
    start = Clock::now();
  }
}

void ReadSeq(ThreadArg* arg) { Scan(arg, true); }
void ReadReverse(ThreadArg* arg) { Scan(arg, false); }

void SeekRandom(ThreadArg* arg) {
  std::mt19937_64 rnd(FLAGS.seed + 3000 + arg->index);
  for (uint64_t i = 0; i < arg->ops; i++) {
    const std::string key = MakeKey(rnd() % FLAGS.num);
    const auto start = Clock::now();
    std::unique_ptr<Cursor> cursor = arg->engine->NewCursor();
    cursor->Seek(key);
    if (cursor->Valid() && cursor->key() == key) arg->stats.found++;
    cursor.reset();
    arg->stats.Record(start);
  }
}

// YCSB core workloads. Proportions per the YCSB distribution:
//   A 50% read, 50% update          B 95% read, 5% update
//   C 100% read                     D 95% read (recent keys), 5% insert
//   E 95% short scan, 5% insert     F 50% read, 50% read-modify-write
void Ycsb(ThreadArg* arg, char workload) {
  std::mt19937_64 rnd(FLAGS.seed + 4000 + arg->index);
  Zipfian zipf(FLAGS.num, FLAGS.seed + 5000 + arg->index);
  ValueSource values(FLAGS.seed + arg->index);
  std::string value;
  // Inserts extend the key space; each thread gets its own range.
  uint64_t next_insert = FLAGS.num + static_cast<uint64_t>(arg->index) * arg->ops;
  uint64_t inserted = 0;

  for (uint64_t i = 0; i < arg->ops; i++) {
    const uint32_t dice = rnd() % 100;
    const auto start = Clock::now();
    bool found = false;
    switch (workload) {
      case 'a':
      case 'b':
      case 'c': {
        const int read_percent = workload == 'a' ? 50 : workload == 'b' ? 95 : 100;
        const std::string key = MakeKey(zipf.Next());
        if (static_cast<int>(dice) < read_percent) {
          if (!arg->engine->Get(key, &value, &found)) arg->stats.errors++;
        } else if (!arg->engine->Put(key, values.Next(), FLAGS.sync)) {
          arg->stats.errors++;
        }
        break;
      }
      case 'd': {
        if (dice < 95) {
          // Read-latest: popular items are the most recently inserted.
          const uint64_t back = zipf.Next() % (inserted + 1);
          const std::string key =
              inserted == 0 ? MakeKey(zipf.Next()) : MakeKey(next_insert - 1 - back % inserted);
          if (!arg->engine->Get(key, &value, &found)) arg->stats.errors++;
        } else {
          if (!arg->engine->Put(MakeKey(next_insert++), values.Next(), FLAGS.sync)) {
            arg->stats.errors++;
          }
          inserted++;
        }
        break;
      }
      case 'e': {
        if (dice < 95) {
          std::unique_ptr<Cursor> cursor = arg->engine->NewCursor();
          cursor->Seek(MakeKey(zipf.Next()));
          for (uint32_t n = 1 + rnd() % 100; n > 0 && cursor->Valid(); n--) {
            arg->stats.bytes += cursor->key().size() + cursor->value().size();
            cursor->Next();
          }
          found = true;
        } else if (!arg->engine->Put(MakeKey(next_insert++), values.Next(), FLAGS.sync)) {
          arg->stats.errors++;
        }
        break;
      }
      case 'f': {
        const std::string key = MakeKey(zipf.Next());
        if (!arg->engine->Get(key, &value, &found)) arg->stats.errors++;
        if (dice >= 50 && !arg->engine->Put(key, values.Next(), FLAGS.sync)) {
          arg->stats.errors++;
        }
        break;
      }
    }
    arg->stats.Record(start);
    if (found) arg->stats.found++;
  }
}

void YcsbA(ThreadArg* arg) { Ycsb(arg, 'a'); }
void YcsbB(ThreadArg* arg) { Ycsb(arg, 'b'); }
void YcsbC(ThreadArg* arg) { Ycsb(arg, 'c'); }
void YcsbD(ThreadArg* arg) { Ycsb(arg, 'd'); }
void YcsbE(ThreadArg* arg) { Ycsb(arg, 'e'); }
void YcsbF(ThreadArg* arg) { Ycsb(arg, 'f'); }

// The writer that runs behind readwhilewriting.
void BackgroundWriter(ThreadArg* arg) {
  std::mt19937_64 rnd(FLAGS.seed + 9000);
  ValueSource values(FLAGS.seed + 9000);
  while (!arg->stop->load(std::memory_order_relaxed)) {
    if (!arg->engine->Put(MakeKey(rnd() % FLAGS.num), values.Next(), false)) arg->stats.errors++;
    arg->stats.ops++;
  }
}

// --- Running and reporting.

double Percentile(const std::vector<uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const size_t index = std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()));
  return sorted[index] / 1000.0;  // microseconds
}

std::string CounterOr(Engine* engine, const std::string& name, const std::string& fallback) {
  std::string value;
  return engine->Counter(name, &value) ? value : fallback;
}

struct RunSpec {
  std::string name;
  BenchFn fn;
  uint64_t ops_per_thread;
  int threads;
  bool with_background_writer = false;
  bool is_write = false;
};

void RunBenchmark(Engine* engine, const RunSpec& spec) {
  std::vector<ThreadArg> args(spec.threads);
  for (int t = 0; t < spec.threads; t++) {
    args[t].index = t;
    args[t].ops = spec.ops_per_thread;
    args[t].engine = engine;
    args[t].stats.latencies_ns.reserve(spec.ops_per_thread);
  }

  std::atomic<bool> stop{false};
  ThreadArg writer_arg{};
  writer_arg.engine = engine;
  writer_arg.stop = &stop;
  std::thread writer;
  if (spec.with_background_writer) writer = std::thread(BackgroundWriter, &writer_arg);

  // Space amplification over time: sample the directory size from a side
  // thread while the benchmark runs.
  std::vector<std::pair<double, uint64_t>> space_series;
  std::atomic<bool> sampling{FLAGS.space_samples > 0 && spec.is_write};
  std::thread sampler;

  const std::string stall_before = CounterOr(engine, "lsm.stall-micros", "0");
  const std::string probed_before = CounterOr(engine, "lsm.files-probed-per-get", "");
  const IoCounters io_before = ReadIoCounters();
  const auto start = Clock::now();

  if (sampling) {
    sampler = std::thread([&] {
      while (sampling.load()) {
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        space_series.emplace_back(seconds, DirectorySize(FLAGS.db));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    });
  }

  std::vector<std::thread> threads;
  for (int t = 0; t < spec.threads; t++) threads.emplace_back(spec.fn, &args[t]);
  for (std::thread& t : threads) t.join();
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();

  stop.store(true);
  if (writer.joinable()) writer.join();
  sampling.store(false);
  if (sampler.joinable()) sampler.join();

  const IoCounters io_after = ReadIoCounters();

  std::vector<uint32_t> latencies;
  uint64_t ops = 0, found = 0, bytes = 0, errors = writer_arg.stats.errors;
  for (ThreadArg& arg : args) {
    latencies.insert(latencies.end(), arg.stats.latencies_ns.begin(), arg.stats.latencies_ns.end());
    ops += arg.stats.ops;
    found += arg.stats.found;
    bytes += arg.stats.bytes;
    errors += arg.stats.errors;
  }
  std::sort(latencies.begin(), latencies.end());
  const double ops_per_sec = seconds > 0 ? ops / seconds : 0;
  const double p50 = Percentile(latencies, 0.50);
  const double p99 = Percentile(latencies, 0.99);
  const double p999 = Percentile(latencies, 0.999);
  const uint64_t written = io_after.bytes_written - io_before.bytes_written;
  const uint64_t read = io_after.bytes_read - io_before.bytes_read;
  const uint64_t stall_micros = std::strtoull(CounterOr(engine, "lsm.stall-micros", "0").c_str(), nullptr, 10) -
                                std::strtoull(stall_before.c_str(), nullptr, 10);

  std::printf("%-18s %10.0f ops/s  %8.2f us/op  p50 %8.2f  p99 %9.2f  p99.9 %10.2f us  "
              "wrote %8.1f MB  read %8.1f MB",
              spec.name.c_str(), ops_per_sec, ops > 0 ? seconds * 1e6 / ops * spec.threads : 0, p50,
              p99, p999, written / 1048576.0, read / 1048576.0);
  if (found > 0 || spec.name.starts_with("read") || spec.name.starts_with("seek")) {
    std::printf("  (%llu of %llu found)", static_cast<unsigned long long>(found),
                static_cast<unsigned long long>(ops));
  }
  if (stall_micros > 0) std::printf("  stalled %.2f s", stall_micros / 1e6);
  if (errors > 0) std::printf("  %llu ERRORS", static_cast<unsigned long long>(errors));
  std::printf("\n");
  std::fflush(stdout);

  if (!FLAGS.json.empty()) {
    if (FILE* f = std::fopen(FLAGS.json.c_str(), "a")) {
      std::fprintf(f,
                   "{\"engine\":\"%s\",\"label\":\"%s\",\"benchmark\":\"%s\",\"threads\":%d,"
                   "\"ops\":%llu,"
                   "\"seconds\":%.6f,\"ops_per_sec\":%.2f,\"p50_us\":%.3f,\"p99_us\":%.3f,"
                   "\"p999_us\":%.3f,\"bytes_written\":%llu,\"bytes_read\":%llu,"
                   "\"user_bytes\":%llu,\"found\":%llu,\"errors\":%llu,\"stall_micros\":%llu,"
                   "\"num\":%llu,\"value_size\":%d,\"sync\":%s,\"group_commit\":%s,"
                   "\"level_size_multiplier\":%d,\"db_bytes\":%llu,"
                   "\"write_amplification\":\"%s\",\"files_probed_before\":\"%s\","
                   "\"files_probed_after\":\"%s\",\"space_series\":[",
                   engine->name(), FLAGS.label.c_str(), spec.name.c_str(), spec.threads,
                   static_cast<unsigned long long>(ops), seconds, ops_per_sec, p50, p99, p999,
                   static_cast<unsigned long long>(written), static_cast<unsigned long long>(read),
                   static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(found),
                   static_cast<unsigned long long>(errors),
                   static_cast<unsigned long long>(stall_micros),
                   static_cast<unsigned long long>(FLAGS.num), FLAGS.value_size,
                   FLAGS.sync ? "true" : "false", FLAGS.config.group_commit ? "true" : "false",
                   FLAGS.config.level_size_multiplier,
                   static_cast<unsigned long long>(DirectorySize(FLAGS.db)),
                   CounterOr(engine, "lsm.write-amplification", "").c_str(), probed_before.c_str(),
                   CounterOr(engine, "lsm.files-probed-per-get", "").c_str());
      for (size_t i = 0; i < space_series.size(); i++) {
        std::fprintf(f, "%s[%.2f,%llu]", i > 0 ? "," : "", space_series[i].first,
                     static_cast<unsigned long long>(space_series[i].second));
      }
      std::fprintf(f, "]}\n");
      std::fclose(f);
    }
  }
}

bool ParseFlag(const std::string& arg, const char* name, std::string* out) {
  const std::string prefix = std::string("--") + name + "=";
  if (!arg.starts_with(prefix)) return false;
  *out = arg.substr(prefix.size());
  return true;
}

int Main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    std::string v;
    if (ParseFlag(arg, "engine", &v)) FLAGS.engine = v;
    else if (ParseFlag(arg, "benchmarks", &v)) FLAGS.benchmarks = v;
    else if (ParseFlag(arg, "db", &v)) FLAGS.db = v;
    else if (ParseFlag(arg, "json", &v)) FLAGS.json = v;
    else if (ParseFlag(arg, "num", &v)) FLAGS.num = std::stoull(v);
    else if (ParseFlag(arg, "reads", &v)) FLAGS.reads = std::stoll(v);
    else if (ParseFlag(arg, "threads", &v)) FLAGS.threads = std::stoi(v);
    else if (ParseFlag(arg, "key_size", &v)) FLAGS.key_size = std::stoi(v);
    else if (ParseFlag(arg, "value_size", &v)) FLAGS.value_size = std::stoi(v);
    else if (ParseFlag(arg, "sync", &v)) FLAGS.sync = v == "1";
    else if (ParseFlag(arg, "use_existing_db", &v)) FLAGS.use_existing_db = v == "1";
    else if (ParseFlag(arg, "seed", &v)) FLAGS.seed = static_cast<uint32_t>(std::stoul(v));
    else if (ParseFlag(arg, "space_samples", &v)) FLAGS.space_samples = std::stoi(v);
    else if (ParseFlag(arg, "writes_per_sec", &v)) FLAGS.writes_per_sec = std::stod(v);
    else if (ParseFlag(arg, "write_buffer_size", &v)) FLAGS.config.write_buffer_size = std::stoull(v);
    else if (ParseFlag(arg, "cache_size", &v)) FLAGS.config.block_cache_size = std::stoull(v);
    else if (ParseFlag(arg, "block_size", &v)) FLAGS.config.block_size = std::stoull(v);
    else if (ParseFlag(arg, "bloom_bits", &v)) FLAGS.config.bloom_bits_per_key = std::stoi(v);
    else if (ParseFlag(arg, "max_file_size", &v)) FLAGS.config.max_file_size = std::stoull(v);
    else if (ParseFlag(arg, "level_base", &v)) FLAGS.config.max_bytes_for_level_base = std::stoull(v);
    else if (ParseFlag(arg, "level_multiplier", &v)) FLAGS.config.level_size_multiplier = std::stoi(v);
    else if (ParseFlag(arg, "group_commit", &v)) FLAGS.config.group_commit = v == "1";
    else if (ParseFlag(arg, "verify_checksums", &v)) FLAGS.config.verify_checksums = v == "1";
    else if (ParseFlag(arg, "label", &v)) FLAGS.label = v;
    else {
      std::fprintf(stderr, "unknown flag: %s\n", arg.c_str());
      return 2;
    }
  }
  if (FLAGS.key_size < 8 || FLAGS.key_size > 60) {
    std::fprintf(stderr, "--key_size must be between 8 and 60\n");
    return 2;
  }

  std::unique_ptr<Engine> engine = FLAGS.engine == "lsm"       ? NewLsmEngine()
                                   : FLAGS.engine == "leveldb" ? NewLevelDBEngine()
                                   : FLAGS.engine == "rocksdb" ? NewRocksDBEngine()
                                                               : nullptr;
  if (engine == nullptr) {
    std::fprintf(stderr, "engine '%s' is unknown or was not built in\n", FLAGS.engine.c_str());
    return 2;
  }

  if (!FLAGS.use_existing_db) std::filesystem::remove_all(FLAGS.db);
  FLAGS.config.path = FLAGS.db;
  if (const std::string error = engine->Open(FLAGS.config); !error.empty()) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("engine %s  keys %d bytes  values %d bytes  entries %llu  threads %d  "
              "write buffer %zu MB  block cache %zu MB  bloom %d bits  sync %d\n",
              engine->name(), FLAGS.key_size, FLAGS.value_size,
              static_cast<unsigned long long>(FLAGS.num), FLAGS.threads,
              FLAGS.config.write_buffer_size >> 20, FLAGS.config.block_cache_size >> 20,
              FLAGS.config.bloom_bits_per_key, FLAGS.sync ? 1 : 0);

  const uint64_t reads = FLAGS.reads < 0 ? FLAGS.num : static_cast<uint64_t>(FLAGS.reads);
  size_t pos = 0;
  while (pos <= FLAGS.benchmarks.size()) {
    const size_t comma = std::min(FLAGS.benchmarks.find(',', pos), FLAGS.benchmarks.size());
    const std::string name = FLAGS.benchmarks.substr(pos, comma - pos);
    pos = comma + 1;
    if (name.empty()) continue;

    RunSpec spec{name, nullptr, 0, FLAGS.threads};
    const uint64_t writes_per_thread = FLAGS.num / FLAGS.threads;
    const uint64_t reads_per_thread = reads / FLAGS.threads;
    if (name == "fillseq") {
      spec.fn = FillSeq, spec.ops_per_thread = FLAGS.num, spec.threads = 1, spec.is_write = true;
    } else if (name == "fillrandom" || name == "overwrite") {
      spec.fn = FillRandom, spec.ops_per_thread = writes_per_thread, spec.is_write = true;
    } else if (name == "fillsync") {
      const bool saved = FLAGS.sync;
      FLAGS.sync = true;
      spec.fn = FillRandom, spec.ops_per_thread = std::max<uint64_t>(FLAGS.num / 1000, 1);
      spec.is_write = true;
      RunBenchmark(engine.get(), spec);
      FLAGS.sync = saved;
      continue;
    } else if (name == "readrandom") {
      spec.fn = ReadRandom, spec.ops_per_thread = reads_per_thread;
    } else if (name == "readmissing") {
      spec.fn = ReadMissing, spec.ops_per_thread = reads_per_thread;
    } else if (name == "readseq") {
      spec.fn = ReadSeq, spec.ops_per_thread = reads;
    } else if (name == "readreverse") {
      spec.fn = ReadReverse, spec.ops_per_thread = reads;
    } else if (name == "seekrandom") {
      spec.fn = SeekRandom, spec.ops_per_thread = reads_per_thread;
    } else if (name == "readwhilewriting") {
      spec.fn = ReadRandom, spec.ops_per_thread = reads_per_thread;
      spec.with_background_writer = true;
    } else if (name.size() == 5 && name.starts_with("ycsb") && name[4] >= 'a' && name[4] <= 'f') {
      static constexpr BenchFn kYcsb[] = {YcsbA, YcsbB, YcsbC, YcsbD, YcsbE, YcsbF};
      spec.fn = kYcsb[name[4] - 'a'], spec.ops_per_thread = reads_per_thread;
      spec.is_write = name[4] != 'c';
    } else if (name == "compact") {
      const auto start = Clock::now();
      engine->CompactAll();
      std::printf("%-18s %.2f s\n", "compact",
                  std::chrono::duration<double>(Clock::now() - start).count());
      continue;
    } else if (name == "waitforbg") {
      const auto start = Clock::now();
      engine->WaitForBackground();
      // Not every engine can say when it is idle. For all of them, also
      // wait until the directory has stopped changing.
      WaitUntilDirectoryIsStable(FLAGS.db);
      std::printf("%-18s %.2f s\n", "waitforbg",
                  std::chrono::duration<double>(Clock::now() - start).count());
      // The settled state: what the writes since the database was opened
      // cost once their compactions were done, and the shape they left.
      std::string level_stats;
      if (!FLAGS.json.empty() && engine->Counter("lsm.level-stats", &level_stats)) {
        std::replace(level_stats.begin(), level_stats.end(), '\n', '|');
        if (FILE* f = std::fopen(FLAGS.json.c_str(), "a")) {
          std::fprintf(f,
                       "{\"engine\":\"%s\",\"label\":\"%s\",\"benchmark\":\"settled\","
                       "\"level_size_multiplier\":%d,\"writes_per_sec\":%.0f,"
                       "\"user_bytes_written\":%s,\"wal_bytes_written\":%s,"
                       "\"write_amplification\":\"%s\",\"db_bytes\":%llu,\"level_stats\":\"%s\"}\n",
                       engine->name(), FLAGS.label.c_str(), FLAGS.config.level_size_multiplier,
                       FLAGS.writes_per_sec,
                       CounterOr(engine.get(), "lsm.user-bytes-written", "0").c_str(),
                       CounterOr(engine.get(), "lsm.wal-bytes-written", "0").c_str(),
                       CounterOr(engine.get(), "lsm.write-amplification", "").c_str(),
                       static_cast<unsigned long long>(DirectorySize(FLAGS.db)),
                       level_stats.c_str());
          std::fclose(f);
        }
      }
      continue;
    } else if (name == "stats") {
      std::printf("%s\n", engine->Stats().c_str());
      continue;
    } else {
      std::fprintf(stderr, "unknown benchmark: %s\n", name.c_str());
      return 2;
    }
    RunBenchmark(engine.get(), spec);
  }

  engine->Close();
  return 0;
}

}  // namespace

}  // namespace bench

int main(int argc, char** argv) { return bench::Main(argc, argv); }
