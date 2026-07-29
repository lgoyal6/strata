# strata — resume material

Every number below is measured, committed to `bench/results/`, and
reproducible with one command. Don't claim anything here you can't re-run.

## Project line

**strata** — LSM-tree storage engine in C++20 with provable crash durability
*(C++20, POSIX, CMake, libFuzzer, ASan/UBSan/TSan, GitHub Actions)*

## Bullets (pick 3–4)

- Built a leveled LSM-tree key-value store from scratch in ~8,600 lines of
  C++20 (zero-dependency core): checksummed write-ahead log, lock-free-reader
  skiplist memtables, prefix-compressed SSTables with Bloom filters and a
  sharded LRU block cache, size-tiered-L0 + leveled compaction with write
  backpressure, MVCC snapshots, and group commit.
- Proved the durability contract with a fault-injection crash harness that
  tears `write(2)` calls at randomized byte offsets and SIGKILLs the engine:
  12,000 crash points (11,149 real kills) across every fsync policy and
  chained crash-recover-crash cycles — 2.2M acknowledged writes verified,
  zero lost, zero torn records accepted; the same 10k+-kill matrix runs in CI
  on every push.
- The harness caught two real bugs pre-release: a recovery-protocol hole
  where a torn WAL tail resurfaced as mid-sequence corruption after chained
  crashes (fixed by durable truncation at recovery), and a 1-in-10⁴
  interleaving where the background fsync tick raced the group-commit
  leader's WAL buffer.
- Fuzzed every recovery-path parser (WAL, SSTable, MANIFEST) with
  coverage-instrumented libFuzzer under ASan/UBSan — 26M+ executions, zero
  findings; test suite also clean under ThreadSanitizer, plus a model-based
  random tester validating scans/point-reads/snapshots against a reference
  map through constant flush/compaction churn and reopens.
- Benchmarked head-to-head against RocksDB on YCSB (matched tuning,
  compression off, 1M keys): beat it single-threaded (1.24× load, 1.17×
  YCSB-A, parity on reads) and published where it loses — 0.65–0.79× at 4
  threads and 0.43× on scans — with root-cause analysis; write amplification
  4.53× vs RocksDB's 4.66× on the identical load; 50× RocksDB's throughput
  on synchronous commits (1,015 vs 20 ops/s, both under `F_FULLFSYNC`) via
  group commit.

## Shorter variant (2 bullets, if space-constrained)

- Built an LSM-tree storage engine in C++20 (WAL, SSTables + Bloom filters,
  leveled compaction, MVCC, group commit) and proved zero acknowledged-write
  loss across 12,000 randomized SIGKILL crash points — including
  byte-granular torn writes injected inside its own `write(2)` calls — with
  the matrix re-run in CI on every push; fault harness surfaced and fixed 2
  real recovery/concurrency bugs.
- Benchmarked against RocksDB on YCSB with matched tuning: 1.04–1.24×
  single-threaded, 4.53× vs 4.66× write amplification, 50× on synchronous
  commits; published the 4-thread and scan losses with root-cause analysis.

## Numbers cheat-sheet (for interviews)

| claim | number | source |
|---|---|---|
| code size | ~5,900 core + ~2,700 test/tools LOC | `wc -l` |
| crash matrix | 12 configs × 1,000 iters; 11,149 SIGKILLs; 2,223,252 acked writes verified; 0 failures | `bench/results/crash_matrix.txt` |
| CI crash gate | 10,200 kill points/push | `.github/workflows/ci.yml` |
| fuzzing | 26M+ execs, 3 targets, cov 413–836 edges, 0 findings | `bench/results/fuzz.txt` |
| unit tests | 46, incl. 30k-op model tester; ASan/UBSan/TSan clean | `ctest` |
| load (1 thread) | 438k ops/s vs RocksDB 354k (1.24×) | `bench/results/ycsb.txt` |
| reads (4 threads) | 1.83M ops/s vs RocksDB 2.67M (0.69×) | same |
| scans | 0.43× RocksDB — worst loss, root-caused | same |
| write amp (1M load) | 4.53× vs RocksDB 4.66× | same |
| sync commits (F_FULLFSYNC, 4T) | 1,015 vs 20 ops/s | same |
| hardware | Apple M3 Pro, APFS, RocksDB 11.1.2 (brew) | BENCHMARKS.md §3 |

## Interview stories (rehearse these)

1. **Torn-tail resurrection** (chained-crash bug): crash tears WAL tail →
   recovery stops at tear, opens new WAL → tear is now mid-sequence → next
   recovery can't tell it from real corruption. Fix: durably `ftruncate` the
   tear during recovery, restoring the invariant "a tear is only ever the
   final record." Found by chain mode in <60 iterations.
2. **Interval-fsync race**: group-commit leader appends to the WAL with the
   DB mutex released; background fsync tick flushed a half-appended buffer →
   CRC-invalid record mid-WAL. Found at kill ~11,700/12,000. Fix: WalWriter
   internal mutex. Lesson: "the sync path and the append path share a
   buffer" is invisible until you kill at byte granularity.
3. **Why replay-all-WALs, no sequence filter**: manifest's `last_sequence`
   includes writes that only ever lived in WAL+memtable; filtering replay by
   it silently drops acked writes. WAL rotation is coupled to memtable
   rotation, so WALs ≥ `min_wal_number` are exactly the unflushed data.
4. **SIGKILL vs power loss**: SIGKILL preserves page cache, so all three
   fsync policies must (and do) show zero loss; power-loss durability is
   claimed only for `fsync=always` (+`F_FULLFSYNC` through the drive cache).
   Most people conflate these; the matrix claims exactly what it tests.
5. **Where it loses and why**: 4-thread losses = single-leader commit +
   DB-mutex source capture (RocksDB pipelines WAL/memtable writes); scan
   loss = eager iterator construction over every live file. Known fixes,
   deliberately out of v1 scope.

## Do-not-overclaim notes

- "Beats RocksDB" only single-threaded and on sync commits; say "competitive
  single-threaded, published losses at 4 threads" — the honesty is the
  point.
- Durability claim is "zero acknowledged-write loss under SIGKILL at 12k
  randomized points" — not "power-loss proven" (that requires pulling
  plugs).
- Bench is one machine, one run per cell, page-cache-resident working set —
  documented in BENCHMARKS.md §8.
