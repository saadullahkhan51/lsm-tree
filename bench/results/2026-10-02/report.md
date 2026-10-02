# Benchmark results

```
date: 2026-10-01T20:44:27Z
os: Darwin 25.5.0 arm64
cpu: Apple M3 Pro
cores: 11
memory_gb: 18
filesystem: apfs
sync: fcntl(F_FULLFSYNC)
page_cache: not dropped between benchmarks
load_average_at_start: 5.13 5.99 6.94
num: 5000000
reads: 1000000
runs: 3
load_average_at_end: 9.36 5.75 5.94
note: other programs were running throughout, and took more than a core for part of the comparison
note: the machine was suspended on and off during the last steps. Affected: the write amplification runs at multiplier 16 and the space amplification run. Their byte counts are exact; their timings are not.
```

## Throughput, operations per second

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillseq | 524k (514k–576k) | 470k (128k–534k) † | 477k (123k–535k) † | 476k (103k–523k) † |
| fillrandom | 267k (265k–346k) | 248k (76.9k–268k) † | 231k (77.4k–293k) † | 281k (76.3k–320k) † |
| overwrite | 238k (229k–298k) | 209k (77.4k–259k) † | 113k (79.9k–263k) † | 278k (74.7k–352k) † |
| fillsync | 351 (343–355) | 339 (338–341) | 336 (329–340) | 350 (347–350) |
| readrandom | 163k (159k–210k) | 172k (75.1k–182k) † | 193k (44k–329k) † | 160k (32.3k–226k) † |
| readmissing | 508k (479k–613k) | 344k (140k–434k) † | 644k (88.3k–654k) † | 438k (164k–608k) † |
| readseq | 4.14M (4.11M–4.64M) | 9.62M (2.95M–9.91M) † | 13.9M (1.83M–13.9M) † | 4.76M (1.09M–5.3M) † |
| readreverse | 3.27M (3.24M–3.69M) | 5.18M (1.84M–5.22M) † | 6.55M (1.19M–6.6M) † | 3.29M (597k–3.68M) † |
| seekrandom | 52k (51.8k–72.6k) | 244k (87.6k–254k) † | 345k (55.4k–429k) † | 77k (11.2k–86.9k) † |
| readwhilewriting | 179k (157k–252k) | 219k (52.5k–272k) † | 231k (32.8k–331k) † | 203k (11.8k–238k) † |

Median of the runs, with the lowest and highest in brackets. LevelDB* is LevelDB with its default of not verifying block checksums on reads. † marks a cell whose runs disagree by more than 2×: something else was using the machine during at least one of them, and the cell should not be used to compare engines.

## Median latency, microseconds

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillseq | 1.54 (1.46–1.58) | 1.67 (1.58–4.58) † | 1.62 (1.58–4.58) † | 1.79 (1.71–5.54) † |
| fillrandom | 2.88 (2.17–2.92) | 3.04 (2.62–8.25) † | 2.96 (2.46–8.25) † | 3.08 (2.67–8.21) † |
| overwrite | 2.96 (2.25–2.96) | 3.25 (2.71–8.08) † | 5.29 (2.58–8.12) † | 3.12 (2.54–8.29) † |
| fillsync | 2.99k (2.98k–2.99k) | 2.99k (2.99k–3k) | 2.99k (2.99k–3k) | 2.99k (2.99k–2.99k) |
| readrandom | 4.25 (3.46–4.29) | 3.5 (3.5–7.88) † | 2.67 (2.21–8.17) † | 4.62 (4.42–11) † |
| readmissing | 1.38 (1.04–1.42) | 2.33 (1.92–5.42) † | 1.33 (1.33–5.83) † | 1.88 (1.33–3.54) |
| readseq | 0.12 (0.12–0.12) | 0.04 (0.04–0.12) † | 0.04 (0.04–0.12) † | 0.12 (0.12–0.25) |
| readreverse | 0.17 (0.17–0.17) | 0.12 (0.12–0.29) † | 0.12 (0.12–0.33) † | 0.17 (0.17–0.42) † |
| seekrandom | 17.7 (13.6–17.9) | 3.46 (3.38–8.25) † | 2.54 (2.21–10.8) † | 12.2 (10.3–37.6) † |
| readwhilewriting | 5.17 (4.17–5.5) | 3.62 (3.33–9.92) † | 3.67 (2.79–11.3) † | 4.71 (4.04–18.7) † |

## 99th percentile latency, microseconds

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillseq | 5 (4–5.17) | 6.62 (4.67–27.5) † | 6.29 (4.42–29.2) † | 5.42 (4.21–34) † |
| fillrandom | 8.54 (5.33–8.71) | 9.58 (9.5–39.8) † | 14.7 (6.83–41) † | 8.62 (6.75–43.5) † |
| overwrite | 8.92 (5.29–9.54) | 14 (6.75–41.6) † | 31.5 (8–40.2) † | 8.62 (5.21–46.8) † |
| fillsync | 3.93k (3.68k–4.07k) | 3.41k (3.4k–3.81k) | 3.7k (3.51k–4.07k) | 3.97k (3.82k–4.04k) |
| readrandom | 93.2 (85.9–107) | 36.4 (24.5–141) † | 29 (6.58–246) † | 88.8 (11.8–256) † |
| readmissing | 8.62 (6.96–9.33) | 10.1 (6.38–20.5) † | 3.29 (2.92–196) † | 7.42 (5.25–30.2) † |
| readseq | 2.33 (2–2.33) | 1.04 (1–2.21) † | 0.25 (0.25–1.71) † | 2.08 (1.83–8.33) † |
| readreverse | 2.58 (2.12–2.58) | 1.25 (1.21–2.75) † | 0.38 (0.33–2.33) † | 2.42 (1.96–15.6) † |
| seekrandom | 45.2 (18.2–50.1) † | 11.8 (10.3–30.2) † | 7 (3.21–214) † | 34.7 (30.1–444) † |
| readwhilewriting | 18.7 (8.71–25) † | 17.2 (9.79–224) † | 15.7 (6.25–292) † | 15.2 (13.2–377) † |

## 99.9th percentile latency, microseconds

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillseq | 16.4 (6.75–20.2) † | 29.4 (12.6–102) † | 32.3 (8.88–139) † | 18.3 (6.96–124) † |
| fillrandom | 33.6 (14.5–33.9) † | 46.7 (42.6–162) † | 55.5 (33.1–186) † | 30.4 (24.7–148) † |
| overwrite | 52 (14.3–53.8) † | 62.9 (39.7–177) † | 202 (49.8–204) † | 31 (8.79–173) † |
| fillsync | 7.23k (7.2k–7.41k) | 7.26k (7.14k–8.43k) | 7.23k (7.11k–21.7k) † | 7.18k (7.13k–7.23k) |
| readrandom | 140 (133–152) | 276 (255–415) | 322 (170–755) † | 156 (33.5–380) † |
| readmissing | 19.5 (16.8–21.9) | 26 (17.6–111) † | 5.75 (5.33–332) † | 20.4 (10.8–264) † |
| readseq | 4.42 (2.46–4.92) | 1.83 (1.67–7.33) † | 0.38 (0.33–91) † | 4.92 (2.38–27.3) † |
| readreverse | 5.42 (2.62–5.79) † | 2.71 (2.33–8.92) † | 1.33 (0.79–94.5) † | 5.21 (2.54–154) † |
| seekrandom | 87.1 (22.5–105) † | 33.6 (30.1–106) † | 23.6 (6.42–342) † | 110 (77.3–1k) † |
| readwhilewriting | 42.1 (14.2–76.7) † | 84.2 (25.6–564) † | 35.5 (15.7–926) † | 57.2 (44.2–2.31k) † |

## Megabytes the process wrote

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillseq | 1.28k (1.28k–1.28k) | 1.26k (1.26k–1.27k) | 1.26k (1.25k–1.27k) | 1.25k (1.25k–1.25k) |
| fillrandom | 1.89k (1.89k–1.89k) | 2.31k (2.31k–2.32k) | 2.31k (2.31k–2.32k) | 1.73k (1.73k–1.73k) |
| overwrite | 3.04k (2.93k–3.09k) | 3.02k (2.99k–3.03k) | 3.01k (2.84k–3.02k) | 2.13k (2.08k–2.69k) |
| fillsync | 120 (120–123) | 124 (123–124) | 123 (122–124) | 123 (123–124) |
| readrandom | 0 (0–0) | 819 (816–821) | 804 (556–820) | 0 (0–0) |
| readmissing | 0 (0–0) | 0 (0–0) | 9.23 (0–265) † | 0 (0–0) |
| readseq | 0 (0–0) | 0 (0–0) | 0 (0–0) | 0 (0–0) |
| readreverse | 0 (0–0) | 0 (0–0) | 0 (0–0) | 0 (0–0) |
| seekrandom | 0 (0–0) | 0 (0–0) | 0 (0–0) | 0 (0–0) |
| readwhilewriting | 895 (750–896) | 655 (595–1.32k) † | 669 (506–2.15k) † | 534 (469–2.62k) † |

Everything passed to a write call while the benchmark ran: log, flushes, and compactions. Compaction still running when a benchmark ends is not counted.

## Megabytes read from the storage device

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillseq | 0.01 (0–0.07) † | 0.14 (0.01–0.19) † | 0.14 (0.02–1.19) † | 0.48 (0.33–6.35) † |
| fillrandom | 141 (104–146) | 220 (203–449) † | 294 (206–503) | 296 (288–558) |
| overwrite | 90.4 (17.3–186) † | 302 (261–921) † | 570 (284–835) † | 353 (214–1.83k) † |
| fillsync | 0.01 (0–0.01) † | 0 (0–0.17) | 0 (0–0.2) | 0 (0–0.24) |
| readrandom | 277 (259–359) | 323 (229–341) | 323 (101–1.66k) † | 209 (0–1.64k) † |
| readmissing | 0.05 (0–0.08) † | 0 (0–8.02) | 0 (0–425) | 0 (0–98.5) |
| readseq | 0.03 (0.03–0.05) | 0 (0–0) | 0 (0–34.1) | 0.02 (0.02–90) † |
| readreverse | 0 (0–0) | 0 (0–0.75) | 0 (0–43.8) | 0 (0–98.4) |
| seekrandom | 0 (0–0) | 0 (0–0) | 0 (0–822) | 0 (0–4.04k) |
| readwhilewriting | 0.09 (0.03–0.2) † | 0.16 (0.14–723) † | 0.1 (0.04–2k) † | 1.9 (0.04–7.21k) † |

Reads the page cache could not serve. The page cache was not dropped between benchmarks, so these are warm-cache numbers.

## Seconds lsm's writers spent stalled

| benchmark | lsm |
|---|---:|
| fillseq | 0 (0–0) |
| fillrandom | 0 (0–0) |
| overwrite | 0 (0–0) |

Included in the throughput above. The other engines do not report theirs through this harness.

## YCSB workloads, operations per second

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| ycsba | 229k (159k–315k) | 116k (62.3k–128k) | 65k (54k–215k) † | 310k (48.5k–392k) † |
| ycsbb | 257k (244k–320k) | 174k (136k–290k) | 140k (99.8k–431k) † | 404k (60.8k–545k) † |
| ycsbc | 416k (395k–659k) | 171k (105k–426k) † | 144k (136k–879k) † | 432k (66.5k–473k) † |
| ycsbd | 1.52M (1.46M–1.71M) | 370k (306k–1.32M) † | 1.36M (339k–1.6M) † | 995k (287k–1.07M) † |
| ycsbe | 2.67k (1.58k–3.15k) | 7.7k (2.27k–12.5k) † | 10.7k (2.07k–27.8k) † | 49.4k (6.53k–62.2k) † |
| ycsbf | 259k (255k–363k) | 77.6k (52k–209k) † | 62.2k (50.5k–325k) † | 225k (33.8k–230k) † |

## 4 KiB values, operations per second

| benchmark | lsm | LevelDB | LevelDB* | RocksDB |
|---|---:|---:|---:|---:|
| fillrandom | 51.6k (26.9k–63.5k) | 53.4k (22.8k–66.2k) † | 62k (25.3k–67.2k) † | 123k (34.3k–138k) † |
| readrandom | 81.8k (35.2k–120k) † | 60.6k (23.8k–240k) † | 224k (37.3k–285k) † | 75.2k (15.8k–118k) † |

![Throughput by engine](comparison.svg)

## Group commit: synced writes per second

| threads | group commit | one sync per write | ratio |
|---|---:|---:|---:|
| 1 | 338 | 338 | 1.0× |
| 2 | 392 | 340 | 1.2× |
| 4 | 779 | 339 | 2.3× |
| 8 | 1.52k | 338 | 4.5× |
| 16 | 3.02k | 338 | 8.9× |
| 32 | 5.33k | 335 | 15.9× |
| 64 | 8.1k | 333 | 24.3× |

![Group commit](group_commit.svg)

## Write amplification by level size multiplier

Bytes written per byte of user data while overwriting a populated database.

| multiplier | levels in use | log + flush | into level 1 | into level 2 | into level 3 | into level 4 and deeper | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4, writing flat out | 5 | 2.15 | 1.25 | 1.63 | 2.43 | 2.70 | 10.16 |
| 8, writing flat out | 4 | 2.15 | 1.26 | 2.97 | 3.81 | 0.21 | 10.40 |
| 10, writing flat out | 4 | 2.15 | 1.25 | 3.41 | 3.55 | 0.00 | 10.36 |
| 16, writing flat out | 3 | 2.15 | 1.25 | 4.76 | 2.65 | 0.00 | 10.82 |
| 4, paced, never stalled | 6 | 2.15 | 1.23 | 1.72 | 2.71 | 3.31 | 11.12 |
| 8, paced, never stalled | 5 | 2.15 | 1.24 | 2.98 | 4.35 | 0.89 | 11.61 |
| 10, paced, never stalled | 5 | 2.15 | 1.24 | 3.49 | 5.11 | 0.03 | 12.02 |

The paced run at multiplier 16 is left out: its writer stalled, so it was not paced.

![Write amplification](write_amp.svg)

## Space amplification

Live data: 232 MB. Largest directory size seen: 1.67× that; at the end: 1.60×.

The machine was suspended for 3,616 seconds during this run; that time is removed from the chart.

![Space amplification](space_amp.svg)

## Table files consulted per Get

A table is consulted when its key range covers the key. Its bloom filter is checked first, and a data block is read only if the filter says the key may be there.

| reads of | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| keys that exist | 0.6% | 8.6% | 7.9% | 7.2% | 54.1% | 21.7% | 0.0% | 0.0% |
| keys that do not | 0.0% | 0.0% | 0.0% | 0.0% | 58.7% | 41.3% | 0.0% | 0.0% |

![Files consulted per Get](files_probed.svg)
