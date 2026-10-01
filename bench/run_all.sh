#!/bin/bash
# Runs the whole benchmark suite and writes raw results as JSON lines.
#
#   bench/run_all.sh [db_bench binary] [output dir] [scratch dir for databases]
#
# Environment:
#   NUM    entries per database             (default 5000000)
#   READS  operations per read benchmark    (default 1000000)
#   RUNS   repetitions of the comparison    (default 3; the report takes medians)
#
# Then: python3 bench/report.py <output dir>
set -euo pipefail

BENCH=${1:-build/release/db_bench}
OUT=${2:-bench/results/$(date +%Y-%m-%d)}
SCRATCH=${3:-/tmp/lsm_bench}
NUM=${NUM:-5000000}
READS=${READS:-1000000}
RUNS=${RUNS:-3}

mkdir -p "$OUT" "$SCRATCH"
RAW="$OUT/raw.jsonl"
: > "$RAW"

# What the numbers were measured on.
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "os: $(uname -srm)"
  if [ "$(uname)" = "Darwin" ]; then
    echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
    echo "cores: $(sysctl -n hw.ncpu)"
    echo "memory_gb: $(( $(sysctl -n hw.memsize) / 1073741824 ))"
    echo "filesystem: $(mount | awk '$3 == "/" {print $4}' | tr -d '(,')"
    echo "sync: fcntl(F_FULLFSYNC)"
  else
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
    echo "cores: $(nproc)"
    echo "memory_gb: $(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1048576 ))"
    echo "filesystem: $(df -T "$SCRATCH" | awk 'NR==2 {print $2}')"
    echo "sync: fdatasync"
  fi
  echo "page_cache: not dropped between benchmarks"
  # Other work on the machine shows up here; an idle machine reads near 1.
  echo "load_average_at_start: $(uptime | sed 's/.*load averages*: //')"
  echo "num: $NUM"
  echo "reads: $READS"
  echo "runs: $RUNS"
} > "$OUT/machine.txt"

# run <engine> <label> <benchmarks> [extra flags...]
run() {
  local engine=$1 label=$2 benchmarks=$3
  shift 3
  echo "--- $label: $benchmarks $*"
  "$BENCH" --engine="$engine" --label="$label" --db="$SCRATCH/$label" --json="$RAW" \
    --num="$NUM" --reads="$READS" --benchmarks="$benchmarks" "$@"
}

# The comparison. Every engine gets the same write buffer (64 MiB), block
# cache (8 MiB), block size, bloom bits, and no compression. LevelDB runs
# twice: verifying block checksums on reads, as the other two do, and with
# its default of not verifying.
for run_index in $(seq 1 "$RUNS"); do
  echo "=== comparison run $run_index of $RUNS"
  for spec in "lsm lsm 1" "leveldb leveldb 1" "leveldb leveldb-noverify 0" "rocksdb rocksdb 1"; do
    set -- $spec
    engine=$1 label=$2 verify=$3
    # Each write benchmark starts from an empty database.
    run "$engine" "$label" fillseq --verify_checksums="$verify"
    run "$engine" "$label" fillsync --verify_checksums="$verify"
    # The read benchmarks share one database, filled in random order and
    # left until its compactions have finished.
    run "$engine" "$label" \
      fillrandom,waitforbg,readrandom,readmissing,readseq,readreverse,seekrandom,readwhilewriting,overwrite \
      --verify_checksums="$verify"
    # YCSB: load, settle, then the six workloads in the order YCSB
    # recommends (D and E insert, so they go last).
    run "$engine" "$label-ycsb" fillrandom,waitforbg,ycsba,ycsbb,ycsbc,ycsbf,ycsbd,ycsbe \
      --verify_checksums="$verify"
    # Large values: 4 KiB, same total volume divided by 8.
    run "$engine" "$label-large" fillrandom,waitforbg,readrandom \
      --verify_checksums="$verify" --value_size=4096 --num=$((NUM / 25)) --reads=$((READS / 5))
  done
done

# Group commit: synced writes, total held constant, split over more and
# more threads, with group commit on and off.
echo "=== group commit"
for threads in 1 2 4 8 16 32 64; do
  for gc in 1 0; do
    run lsm "groupcommit-$gc" fillrandom --sync=1 --num=6400 --threads="$threads" --group_commit="$gc"
  done
done

# Write amplification against the level size multiplier. A small write
# buffer and level-1 target, so that a couple of hundred megabytes fill
# several levels. One process fills the database; a second overwrites it,
# so that its counters cover only writes into a populated tree. The
# overwrite runs twice: flat out, and paced well below what compaction can
# absorb, to show whether falling behind changes the answer.
echo "=== write amplification"
for multiplier in 4 8 10 16; do
  shape="--num=${WAMP_NUM:-2000000} --write_buffer_size=1048576 --max_file_size=262144 --level_base=1048576 --level_multiplier=$multiplier"
  for pace in 0 8000; do
    run lsm "wamp-fill" fillrandom,waitforbg $shape
    run lsm "wamp-fill" overwrite,waitforbg $shape --use_existing_db=1 --writes_per_sec="$pace" \
      --label="wamp-pace$pace"
  done
done

# Space amplification over time: fill, then overwrite everything three
# times, sampling the size of the directory as it goes.
echo "=== space amplification"
run lsm space fillseq,overwrite,overwrite,overwrite \
  --num=2000000 --write_buffer_size=4194304 --space_samples=1

echo "load_average_at_end: $(uptime | sed 's/.*load averages*: //')" >> "$OUT/machine.txt"
echo "raw results: $RAW"
