# strata benchmarks - method and results

> **Honesty contract.** Both sides of every trade get published. strata is
> ~6k lines written in a week; RocksDB is a decade of production tuning. A
> table where strata wins everything would mean the benchmark is broken  - 
> the interesting part is *where* it loses and *why*.

## 1. Headline

See §7. Summary: strata is competitive on write-heavy workloads (the LSM
shape does the heavy lifting), loses on read-heavy and scan workloads, and
the write-amplification of its leveled compaction lands in the same band as
RocksDB's on the identical load.

## 2. What we measure

- **Throughput** (ops/s) and **latency percentiles** (p50/p95/p99/p999 µs)
  per YCSB workload.
- **Write amplification** = (WAL + flush + compaction bytes written) /
  user payload bytes, from each engine's own counters, on the load phase.
- **Crash durability** is *not* a benchmark: see the crash matrix in the
  README (12,000 SIGKILL points, zero acknowledged-write loss).

## 3. Environment

- Apple M3 Pro (11 cores), 18 GB, APFS on the internal NVMe SSD, macOS 26.5.
- Both engines run in-process through the same driver
  (`bench/bench_main.cc`), same key/value generator, same seed.
- strata `release` build (clang, `-O3`); RocksDB 11.1.2 via Homebrew.

## 4. Exact commands

```
cmake --preset release -DSTRATA_BUILD_BENCH=ON
cmake --build --preset release
./bench/run_matrix.sh          # writes bench/results/ycsb.txt (committed)
```

## 5. Baselines and fairness

Matched between engines (everything else at each engine's defaults):

| knob | value |
|---|---|
| compression | **off** in both (strata has none in v1) |
| write buffer | 8 MiB |
| block size / cache | 4 KiB / 64 MiB LRU |
| bloom filter | 10 bits/key |
| L0 triggers (compact/slowdown/stop) | 4 / 8 / 12 |
| target file size / L1 size | 8 MiB / 64 MiB |
| background threads | 2 (strata: flush + compaction; RocksDB: `max_background_jobs=2`) |
| WAL | on in both; `sync` matched per run |

Method notes:

- YCSB core workloads, scrambled-zipfian θ=0.99 over 1 M records,
  100 B values, 16+4 B keys (`user` + 16 hex).
- Workloads: **load** (1 M inserts), **A** 50/50 read/update, **B** 95/5,
  **C** 100% read, **E-lite** 95% scan(≤100)/5% insert - 1 M ops each
  (200 k for E), threads ∈ {1, 4}.
- Run phases execute against the load-phase directory after a reopen; no
  manual compaction between load and run (both engines settle on their own).
- strata's write-amp counters are exact (`DbStats`); RocksDB's come from its
  `rocksdb.stats` property (0.01 GB resolution) so timing runs carry zero
  statistics overhead in both engines.
- Latency is measured per-op with `steady_clock` around each engine call and
  aggregated across threads; ops/s is total ops over wall time.

## 6. Results

Raw output: [`bench/results/ycsb.txt`](../bench/results/ycsb.txt); the
README holds the transcribed tables. Summary of the measured run
(2026-07-27, idle machine):

- **Single-threaded: strata wins everything.** load 1.22×, A 1.24×,
  B 1.30×, C 1.16×, E 1.43×. E was 0.43× before the scan fix below.
- **Four threads: strata loses everything except scans.** load 0.83×,
  A 0.69×, B 0.75×, C 0.71×, E 1.08×. E was 0.47× before the fix. strata's own 4-thread load throughput is
  *below* its 1-thread number (293 k vs 439 k ops/s) - the single-leader
  commit path and the DB-mutex source capture are the bottleneck, not the
  storage format.
  **Caveat on the 4-thread E ratio specifically: this host cannot resolve
  it.** Repeating the 4-thread workload-E ratio on a 1 M-record store gave
  **1.28× in strata's favour in one quiet window and 0.58×, against strata,
  in another**, and a third block showed a 1.8× spread within a single
  engine and configuration. The 1.08× above is one run of the matrix and is
  reported for completeness, not as a resolved result; the single-threaded
  comparisons are the stable ones. Fixing this needs a quiet machine with
  more cores, not another run here.
- **Write amplification, identical 1 M-record load:** strata 4.53×
  (137 MB WAL + 116 MB flush + 264 MB compaction / 114 MB payload),
  RocksDB 4.66× (0.13 GB WAL + 0.111 GB flush + 0.29 GB compaction /
  0.114 GB ingest, from `rocksdb.stats`).
- **Synchronous commits (both engines `F_FULLFSYNC`, 4 threads):** strata
  1,015 ops/s vs RocksDB 20 ops/s on the matched small-buffer config; both
  sit at ~4 ms per drive-cache flush, so the difference is how many
  commits share a flush. strata's plain-`fsync` mode (85 k ops/s) is
  reported separately because macOS `fsync` does not flush the drive
  cache - a durability-semantics footnote most benchmarks skip.

## 7. Why we lose where we lose (the part that matters in an interview)

1. **Write/read concurrency (all 4-thread losses).** strata's group commit
   serializes WAL append + memtable apply behind a single leader, and every
   read takes the DB mutex once to capture {memtable, immutables, version}.
   RocksDB pipelines WAL and memtable writes, supports concurrent memtable
   writers, and reads through a lock-free SuperVersion. The fix path is
   known (pipelined commit, per-shard source capture) and deliberately out
   of v1 scope.
2. **Scans: FIXED, and this diagnosis was wrong.** The claim used to be
   that `new_iterator` eagerly opens a cursor on the memtables and every
   live table file, and that a short zipfian scan amortizes that badly.
   Instrumenting it (`-DSTRATA_SCAN_PROBE=ON`) showed otherwise: a scan
   builds **4 children, one of them the single L0 file**, so construction
   is nearly free. The cost was that a scan returning about 50 rows made
   **522 internal advances, 471 of them stepping over superseded versions
   of keys it had already yielded**, one hot key carrying 4,077 of them.
   `skipped_seq = 0`, so snapshot visibility contributed nothing. Zipfian
   updates pile versions on a small hot set and nothing reclaims them
   during a read-heavy phase, which is why only the tail broke: baseline
   p50 already matched RocksDB. `DBIter` now replaces a long version run
   with one targeted seek past the yielded key. Scan p95 170 µs → 15.8 µs,
   p99 186 µs → 19.2 µs, throughput 2.92×. Raw output:
   [`bench/results/ycsb_skipfix_matrix.txt`](../bench/results/ycsb_skipfix_matrix.txt).
   The write-side cause is untouched: this bounds the read symptom, it does
   not make compaction reclaim versions sooner.
3. **Read tails (p99 1.1–2×).** Whole-file bloom filters mean one filter
   miss probes a full index + block; RocksDB's partitioned filters and
   pinned-handle block cache keep its tail flatter.
4. **No compression** in strata v1 costs disk footprint, not speed, with
   both engines set to `kNoCompression` - the fair-comparison choice.

Where strata *wins* is also explainable rather than magic: single-threaded
writes have a shorter code path than RocksDB's (no column families, no
version chains, no statistics), and the sync-commit table is group commit
doing its one job.

## 8. Threats to validity

- Single machine, single run per cell (variance on an idle M3 Pro measured
  informally at <5% run-to-run for these sizes).
- macOS `fsync` does not flush the drive cache; the sync-write comparison
  uses the same `fsync` semantics in both engines (`F_FULLFSYNC` off), so
  the *relative* numbers stand.
- 1 M × 100 B ≈ 116 MB working set fits page cache: this measures engine
  CPU + I/O-path overhead, not disk seeks - deliberately, since both
  engines share the page cache advantage equally.
