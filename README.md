# strata

A leveled LSM-tree key-value store in C++20 with **provable crash
durability**: write-ahead log, skiplist memtables, prefix-compressed
SSTables with Bloom filters, size-tiered L0 + leveled compaction with write
backpressure, MVCC snapshots - and a crash harness that SIGKILLs the engine
at randomized byte offsets inside its own `write(2)` calls and proves that
no acknowledged write is ever lost and no torn record is ever accepted.

![The crash harness killing the engine mid-write a thousand times, every acknowledged write recovered](docs/demo.gif)

A thousand iterations of the crash harness, most of them ending in a real
SIGKILL at a random byte offset inside the engine's own `write(2)`, with every
acknowledged write recovered afterwards. Reproduce it with
`./docs/demo-setup.sh && vhs docs/demo.tape`. Note the honest scope: this kills
the process, not the machine, so it verifies the engine's contract rather than
the drive's.

**Live demo:** [lgoyal6.github.io/strata](https://lgoyal6.github.io/strata/), the
real engine compiled to WebAssembly: write to it, cut its power mid-write, and
watch recovery bring back every acknowledged byte.

> **Thesis.** In a storage engine the interesting property is not speed,
> it's the *contract*: an acknowledged write exists after any crash, and
> recovery never invents data. strata makes that contract mechanically
> checkable - every byte on disk is either CRC-guarded or reconstructible,
> and the whole recovery surface (WAL, SSTable, MANIFEST parsers) is
> fuzzed and crash-swept. Both sides of every benchmark trade get
> published; a table where strata beat RocksDB at everything would mean
> the benchmark is broken.

## The crash matrix

12 configurations × 1,000 iterations, run on every CI push
(`tools/crash_test`). Each iteration forks a child workload, kills it with
a real `SIGKILL` - either at a random **byte offset inside a write(2)**
(deterministic torn writes via the Env fault-injection choke point) or at
a random **wall-clock instant** - then reopens the database and asserts:
(a) every acknowledged op survives with the correct value, (b) every
recovered value passes its embedded checksum, (c) the recovered state
equals the model at *exactly* the acknowledged prefix (± the single
in-flight op). `chain` mode keeps crashing and re-recovering the same
directory, so kills land inside recovery, flush, and compaction of real
state.

| mode | kill | fsync | iterations | real SIGKILLs | acked writes verified | failures |
|---|---|---|---:|---:|---:|---:|
| fresh | byte-offset | always   | 1000 | 871 | 95,821 | **0** |
| fresh | byte-offset | interval | 1000 | 852 | 100,128 | **0** |
| fresh | byte-offset | never    | 1000 | 860 | 100,443 | **0** |
| fresh | timer       | always   | 1000 | 983 | 186,610 | **0** |
| fresh | timer       | interval | 1000 | 919 | 345,111 | **0** |
| fresh | timer       | never    | 1000 | 911 | 357,666 | **0** |
| chain | byte-offset | always   | 1000 | 974 | 48,749 | **0** |
| chain | byte-offset | interval | 1000 | 976 | 49,719 | **0** |
| chain | byte-offset | never    | 1000 | 976 | 49,589 | **0** |
| chain | timer       | always   | 1000 | 992 | 181,438 | **0** |
| chain | timer       | interval | 1000 | 918 | 351,790 | **0** |
| chain | timer       | never    | 1000 | 917 | 356,188 | **0** |
| **total** | | | **12,000** | **11,149** | **2,223,252** | **0** |

Raw output: [`bench/results/crash_matrix.txt`](bench/results/crash_matrix.txt).

Note what the matrix does and doesn't claim: `SIGKILL` preserves the OS
page cache, so **all three fsync policies** must show zero loss (the WAL is
`write(2)`-flushed before every ack) - and they do. Power-loss durability
is additionally claimed only for `fsync=always` (with `use_fullfsync` on
macOS to defeat the drive cache); SIGKILL cannot test that, so the matrix
doesn't pretend to.

### Bugs the harness actually caught

The matrix is not decoration - before it went green it found two real bugs
in this engine (see `docs/DESIGN.md` §1.3, §2.2):

1. **Torn-tail resurrection.** Crash tears the WAL tail → recovery stops at
   the tear and opens a new WAL → the torn bytes are now *mid-sequence* →
   the next crash's recovery cannot distinguish them from real corruption.
   Fix: recovery durably truncates the tear it stops at. Found by `chain`
   mode within 60 iterations.
2. **Interval-fsync buffer race.** The group-commit leader appends to the
   WAL with the DB mutex released; the background fsync tick could flush a
   half-appended buffer, leaving a CRC-invalid record mid-WAL. A
   one-in-thousands interleaving - found at kill point ~11,700 of 12,000.

## Architecture

```
              write(k,v)                       get(k) / scan
                  │                                 │
        ┌─────────▼──────────┐             ┌────────▼─────────┐
        │  writer queue      │             │ MVCC snapshot    │
        │  (group commit)    │             │ (seq pinning)    │
        └───┬──────────┬─────┘             └────────┬─────────┘
            │          │                            │
   ┌────────▼───┐  ┌───▼────────────┐      memtable → immutables
   │  WAL       │  │ memtable       │       → L0 (newest first)
   │  crc32c    │  │ (skiplist)     │       → L1..L6 binary search
   │  records   │  └───┬────────────┘       bloom → index → block
   └────────────┘      │ full: rotate (with WAL)
                  ┌────▼─────────┐    ┌──────────────────┐
                  │ flush thread │    │ compaction thread│
                  │ imm → L0 SST │    │ L0 tiered → L1+  │
                  └────┬─────────┘    │ leveled, cursor  │
                       │              └───────┬──────────┘
                  ┌────▼──────────────────────▼───┐
                  │ MANIFEST: full-snapshot +     │
                  │ atomic rename + dir fsync     │
                  └───────────────────────────────┘
```

- **WAL**: one checksummed record per atomically-committed batch; file
  header carries a per-database UUID; torn tails truncated at recovery.
  fsync policy `always` / `interval` / `never` - the durability matrix
  above is exactly this knob.
- **SSTables**: 4 KiB prefix-compressed blocks with restart points, per-block
  CRC32C, whole-file Bloom filter (10 bits/key), block index with shortest
  separators, fixed 56-byte footer. Proposed format: `docs/DESIGN.md` §1.1.
- **Compaction**: size-tiered L0 (all L0 files merge at once) + leveled
  L1..L6 with a round-robin cursor, boundary-key expansion (the LevelDB
  boundary bug), snapshot-aware GC, tombstones dropped only at the
  bottommost level. Writes **stall rather than OOM**: 1 ms slowdown at 8 L0
  files, hard stop at 12 or 2 immutable memtables.
- **MVCC**: sequence-tagged internal keys; snapshots pin a sequence;
  iterators are point-in-time; compaction never GCs a version a live
  snapshot can still see.
- **Durability ordering** (each step durable before the next): SST fsync →
  dir fsync → MANIFEST tmp fsync → atomic rename → dir fsync → only then
  delete WALs/SSTs. An acknowledged write is always in ≥1 durable place.

## Use it in your project

Both paths land on the same `strata::strata` target, so you can switch between
them without touching your code.

```cmake
include(FetchContent)
FetchContent_Declare(strata
    GIT_REPOSITORY https://github.com/lgoyal6/strata.git
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(strata)

target_link_libraries(your_app PRIVATE strata::strata)
```

Or install it once and find it from anywhere:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target strata
cmake --install build --prefix /usr/local
```

```cmake
find_package(strata 0.1 REQUIRED)
target_link_libraries(your_app PRIVATE strata::strata)
```

Pulled in as a subproject, strata builds the library and nothing else: the
crash harness, tools, fuzzers and the RocksDB benchmark all default off unless
strata is the top-level project, so your configure step never needs clang or a
RocksDB install.

```cpp
#include <strata/db.h>
#include <strata/options.h>

strata::Options opts;
opts.create_if_missing = true;
strata::DB* db = nullptr;
auto st = strata::DB::open(opts, "/tmp/mydb", &db);

st = db->put(strata::WriteOptions{}, "key", "value");
std::string out;
st = db->get(strata::ReadOptions{}, "key", &out);
delete db;
```

The durability contract is the reason to reach for this rather than a map on
disk. `Options::fsync_policy` defaults to `FsyncPolicy::kAlways`, which fsyncs
the WAL before every acknowledgement, and under that policy an acknowledged
`put` survives a `SIGKILL` at any byte offset inside the engine's own
`write(2)`. That is the property the crash matrix below measures. Relax it to
`kInterval` and you trade the guarantee for throughput, deliberately and
visibly.

## Build & test

```
cmake --preset release && cmake --build --preset release
ctest --preset release                    # 46 tests incl. model-based fuzzer
cmake --preset dev && ...                 # ASan/UBSan (Linux/CI)
cmake --preset dev-mac && ...             # ASan/UBSan via brew LLVM (macOS)
```

The highest-leverage test is `test/unit/db_model_test.cc`: 30k random ops
mirrored into a `std::map`, with full-scan/point-get/snapshot equivalence
checked continuously under 4 KiB write buffers (constant flush/compaction
churn) and periodic reopen-recovery. The suite also passes under
ThreadSanitizer (`-DSTRATA_SANITIZE=thread`).

## Crash harness

```
./build/release/tools/crash_test orchestrate \
    --dir /tmp/strata-crash --iters 1000 --workers 8 \
    --fsync always --mode chain --kill bytes --seed 42
```

`--kill bytes` sets `STRATA_CRASH_AT_BYTES=<n>`: the engine's Env counts
every byte handed to `write(2)` and the write crossing byte *n* is torn at
exactly that offset before the process `raise(SIGKILL)`s itself - a
deterministic torn write at an arbitrary byte position (WAL record, SSTable
block, MANIFEST - wherever the offset lands). A failing iteration prints its
exact parameters, and `crash_test repro --seed ... --ops ... --crash-at ...`
replays a byte-mode kill deterministically.

## Fuzzing

`fuzz/fuzz_wal.cc`, `fuzz_sstable.cc`, `fuzz_manifest.cc` - every parser on
the recovery path - run under libFuzzer + ASan/UBSan with
coverage-instrumented library code and corpora seeded from real files:

```
./fuzz/run_fuzz.sh wal 300
```

Byte-level robustness is also unit-tested directly: truncate-at-every-byte
(WAL, MANIFEST) and flip-every-byte (SSTable, MANIFEST) sweeps assert that
corruption is always detected, never silently accepted.

## Benchmarks vs RocksDB

YCSB core workloads, 1 M records × 100 B values, scrambled zipfian θ=0.99,
1 M ops (200 k for E), Apple M3 Pro / APFS. Matched knobs (compression off
in both, same buffers/cache/bloom/L0 triggers - full fairness table in
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md)); RocksDB 11.1.2. Raw output:
[`bench/results/ycsb.txt`](bench/results/ycsb.txt).

| workload | threads | strata ops/s | RocksDB ops/s | strata/RocksDB |
|---|---:|---:|---:|---:|
| load (1M inserts) | 1 | 438,529 | 353,679 | **1.24×** |
| A (50/50 r/w)     | 1 | 495,959 | 425,261 | **1.17×** |
| B (95/5)          | 1 | 694,585 | 628,504 | **1.11×** |
| C (read-only)     | 1 | 798,724 | 764,631 | 1.04× |
| E (95% scans)     | 1 | 33,907  | 79,098  | **0.43×** |
| load              | 4 | 292,968 | 404,424 | **0.72×** |
| A                 | 4 | 449,064 | 694,836 | **0.65×** |
| B                 | 4 | 1,548,368 | 1,949,827 | 0.79× |
| C                 | 4 | 1,831,395 | 2,666,157 | **0.69×** |
| E                 | 4 | 130,463 | 275,785 | **0.47×** |

Write amplification on the identical load (each engine's own counters):
**strata 4.53×, RocksDB 4.66×** - the leveled-compaction cost model lands
where it should.

Synchronous commits (50 k inserts, 4 threads, WAL sync per commit,
`F_FULLFSYNC` in **both** engines - RocksDB always uses it for `sync=true`
on macOS):

| engine / mode | ops/s | p50 |
|---|---:|---:|
| strata, `fsync=always` + `use_fullfsync` | **1,015** | 3.5 ms |
| RocksDB, `sync=true` | 20 | 4.0 ms (p999 298 ms) |
| strata, `fsync=always` (plain `fsync`, weaker: kernel-ordered only) | 85,011 | 50 µs |

Group commit is doing exactly its job: at 3.5–4 ms per drive-cache flush,
throughput is set by how many commits share one flush.

**Where strata loses, and why** (the interview part  - 
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) §7):

- **Every 4-thread workload (0.65–0.79×).** strata's writer queue has a
  single leader doing WAL append + memtable apply; reads contend on one DB
  mutex for source capture. RocksDB pipelines WAL and memtable writes and
  spent a decade shaving its read hot path. strata's single-thread *wins*
  flip to losses exactly when concurrency enters - that's the design gap,
  not noise (strata's own 4-thread load is *slower* than its 1-thread load).
- **Scans (0.43–0.47×, p95 181 µs vs 23 µs).** Each strata scan builds a
  fresh merging iterator that eagerly opens a cursor on every live file;
  RocksDB's iterators are lazier and its per-`Next()` path is specialized.
  Forward-only iteration doesn't excuse this; iterator construction cost
  does most of the damage.
- **Read tails.** p99 on read-heavy workloads runs 1.2–2× RocksDB's
  (whole-file bloom vs partitioned filters; one shared LRU vs sharded,
  pinned cache handling).

## Design document

[`docs/DESIGN.md`](docs/DESIGN.md) - the on-disk formats (proposed before
any code was written), the recovery invariants, the compaction policy and
its write-amplification model, and the durability taxonomy (SIGKILL vs
power loss vs drive cache).

## Limitations (deliberate)

- Forward-only iterators (`Prev()` is absent, not half-implemented).
- No block compression (benchmarks run RocksDB with compression off for
  fairness; snappy/zstd is the obvious v2 item).
- Full-snapshot MANIFEST rewrite per version change - right at embedded
  scale, wrong at RocksDB scale (their log-structured VersionEdit is the
  scale-up path).
- Whole-file Bloom filters, no partitioned indexes, no column families, no
  transactions beyond the atomic `WriteBatch`.
- macOS `fsync` does not flush the drive cache; `Options::use_fullfsync`
  exists and is off by default, matching RocksDB - stated rather than
  hidden.

## License

MIT
