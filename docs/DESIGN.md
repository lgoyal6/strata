# strata — design document

A leveled LSM-tree key-value store in C++20. Single-node, embedded (a
library plus tools), no external dependencies in the core. The design
goal, in order: **provable crash durability**, then read/write
performance, then simplicity of the recovery story — every byte on disk
is either checksummed or reconstructible.

This document proposes the on-disk formats first (they are the contract
everything else must honor), then the in-memory structures, compaction
policy, MVCC, durability model, and the verification strategy (crash
harness, fuzzers, benchmarks).

---

## 1. On-disk formats

All integers are little-endian. All variable-length integers are
LEB128-style varints (7 bits per byte, high bit = continuation), 32- or
64-bit. All checksums are CRC32C (Castagnoli), hardware-accelerated on
arm64/x86-64 with a software fallback, and **masked** LevelDB-style
(rotate + constant) so that a CRC stored alongside the data it covers
does not checksum to itself when files embed other files.

A database directory looks like:

```
db/
  MANIFEST          current version state (atomically replaced)
  000007.wal        write-ahead log segments (monotonic file numbers)
  000012.sst        SSTables
  000013.sst
  LOCK              advisory flock() to prevent double-open
```

File numbers are monotonic across all file types and never reused, so a
stale file can never be mistaken for a live one.

### 1.1 SSTable format (proposal)

An SSTable is an immutable, sorted map from **internal key** to value.

```
internal_key := user_key ⧺ tag(8B)
tag          := (sequence << 8) | value_type      // packed uint64, LE
value_type   := 0x01 = Put, 0x00 = Delete (tombstone)
ordering     := user_key ascending, then sequence descending
                (newest version of a key sorts first)
```

File layout, in write order:

```
+---------------------------+
| data block 0              |   target ~4 KiB before trailer
| data block 1              |
| ...                       |
| data block N-1            |
+---------------------------+
| filter block              |   one Bloom filter for the whole file
+---------------------------+
| index block               |   separator key -> data block handle
+---------------------------+
| footer (fixed 56 bytes)   |
+---------------------------+
```

**Block encoding (shared by data and index blocks).** Entries are
prefix-compressed against the previous key, with a full key stored
every `restart_interval` (16) entries so a block supports binary
search without decoding from the start:

```
entry     := varint32 shared_len
           | varint32 unshared_len
           | varint32 value_len
           | unshared key bytes [unshared_len]
           | value bytes        [value_len]

block     := entry*
           | uint32 restart_offset [num_restarts]   // offsets of entries
           | uint32 num_restarts                    //   with shared_len=0

on disk   := block | uint8 compression_type (0=none) | uint32 crc32c
```

The CRC covers the block bytes plus the compression byte. Every block
read verifies the CRC before parsing (readers must assume the bytes are
adversarial; the fuzz targets enforce this). Lookup within a block:
binary-search the restart array, then linear-scan forward decoding
prefixes. `compression_type` is reserved — v1 ships prefix compression
only; benchmarks against RocksDB disable its block compression to keep
the comparison honest.

**Index block.** One entry per data block. The key is a *shortest
separator* `s` with `last_key(block_i) <= s < first_key(block_i+1)`
(shortens the index; e.g. between `"blueberry"` and `"cat"` the
separator is `"c"`... actually `"bm"`-style first-mismatch+1). The value
is a `BlockHandle`:

```
BlockHandle := varint64 offset | varint64 size    // size excludes trailer
```

`Get(k)` binary-searches the index for the first entry with
`separator >= k`, reads exactly one data block.

**Filter block.** A single Bloom filter over the **user keys** of the
file, ~10 bits per key, k = 7 probes, double hashing (h1 + i*h2) over a
64-bit base hash. False-positive rate ~1%. Stored as:

```
filter := uint32 num_probes | bit array
```

with the standard block trailer (compression byte + CRC). Whole-file
(not per-block) filters keep v1 simple; the footprint at 10 bits/key is
small enough to keep resident.

**Footer** — fixed size, always the last 56 bytes:

```
footer := BlockHandle filter_handle   (padded)   \
        | BlockHandle index_handle    (padded)    | 40 bytes, zero-padded
        | uint32 version   (= 1)
        | uint32 crc32c    (masked, over the 44 bytes above)
        | uint64 magic     (= 0x5354524154414231, "STRATAB1")
```

Open sequence: read last 56 bytes → check magic → check footer CRC →
read + CRC-check index and filter blocks. A truncated, torn, or
bit-flipped SSTable fails loudly at open or at first block read; it can
never silently serve wrong data. SSTables are written to their final
name, fsync'd, and only then referenced by the MANIFEST — so a torn
SSTable can only exist as an *orphan* that recovery deletes (§1.3).

### 1.2 WAL format

A WAL segment starts with a fixed 16-byte header, then a sequence of
self-delimiting, checksummed records. One record = one
atomically-committed write batch.

```
header  := uint64 magic (= 0x5354524154415731, "STRATAW1")
         | uint64 db_uuid          // random at DB creation; also in MANIFEST

record  := uint32 crc32c (masked)   // over type byte + payload
         | uint32 length            // payload length
         | uint8  type              // 0x01 = kFullBatch
         | payload [length]

payload := uint64 first_sequence
         | uint32 count
         | count × ( uint8 op            // 1=Put, 0=Delete
                   | varint32 key_len   | key bytes
                   [ varint32 value_len | value bytes ]   // Put only
                   )
```

The payload is byte-identical to the in-memory `WriteBatch`
serialization, so commit is a single buffer append. Operation *i* in
the batch has sequence `first_sequence + i`; a batch commits atomically
or not at all.

The `db_uuid` (random 8 bytes minted at DB creation, recorded in the
MANIFEST) makes it impossible to replay a WAL from a different database
generation even if file numbers were ever confused.

**Torn-write handling.** Replay reads records sequentially. A record is
accepted iff the header is complete, `length` fits within the remaining
file, and the CRC matches. The first failure is treated as the torn
tail of the log: replay stops there and the file is truncated
logically. Because segments are written once and never recycled,
trailing garbage can only be the result of a torn final write — there
is no stale-record ambiguity, and the CRC mask plus monotonic file
numbers close the recycled-name hole anyway. **A torn or corrupt record
is never applied**; the crash harness and `fuzz_wal` both enforce this.

Regardless of fsync policy, the WAL's userspace buffer is flushed to
the kernel with `write(2)` before every acknowledgement — "fsync=never"
means *no fsync*, never *acked data still in userspace*. This is what
makes all three policies SIGKILL-durable.

**fsync policy** (per-DB option, the core of the durability matrix):

| policy     | behavior                                            | guarantee                                   |
|------------|-----------------------------------------------------|---------------------------------------------|
| `always`   | `fsync()` (or `F_FULLFSYNC`) before acking each batch | acked writes survive power loss             |
| `interval` | background thread syncs every `wal_sync_interval_ms` | acked writes survive process death (SIGKILL); bounded window vs power loss |
| `never`    | no explicit sync; kernel writeback only              | acked writes survive process death (SIGKILL) only |

On macOS `fsync(2)` orders writes to the drive but does not flush the
drive cache; `F_FULLFSYNC` does. `Options::use_fullfsync` selects it
(off by default, matching RocksDB's default on macOS; the design doc is
explicit about this so the durability claim is precise). Note SIGKILL
does not destroy the page cache, so *all three* policies must show zero
acked-write loss in the SIGKILL matrix — the policies differ only in
their power-loss window. See §6.

### 1.3 MANIFEST

The MANIFEST records the *version state*: which SSTables exist at which
levels, the next file number, the last sequence number durable in
SSTables, and the oldest WAL still needed. It is small (tens of files ⇒
a few KiB), so strata uses **full-snapshot-rewrite with atomic rename**
instead of a log-structured manifest:

```
write MANIFEST.tmp   :=  uint32 crc32c | uint32 length | payload
fsync(MANIFEST.tmp)
rename(MANIFEST.tmp, MANIFEST)        // atomic on POSIX
fsync(directory fd)

payload := uint32 version
         | uint64 next_file_number
         | uint64 last_sequence        // max seq persisted in SSTs
         | uint64 min_wal_number       // WALs below this are obsolete
         | per level: uint32 level | uint32 num_files
             | num_files × ( uint64 file_number | uint64 file_size
                           | varint32 len | smallest internal key
                           | varint32 len | largest  internal key )
```

Every metadata transition (flush completes, compaction completes) is
one atomic rename: the system moves between two fully-consistent
states, and there is no manifest-replay code to get wrong. The trade is
O(files) rewrite per transition — irrelevant at embedded scale and an
explicitly documented scalability limit (RocksDB's log-structured
VersionEdit design is the scale-up path).

**Recovery** (single code path, no modes):

1. `flock(LOCK)`; read MANIFEST (CRC-checked). Missing MANIFEST ⇒ new
   DB (an empty MANIFEST is written *before* the first WAL). Stale
   `MANIFEST.tmp` is deleted.
2. Delete any `*.sst`/`*.tmp` in the directory not referenced by the
   MANIFEST — these are orphans from a crash mid-flush/compaction,
   acknowledged to no one.
3. Replay **every record** of every `*.wal` with file number ≥
   `min_wal_number`, in file-number order, stopping at the first torn
   record per §1.2. There is **no sequence filter**: WAL rotation is
   coupled to memtable rotation, so WALs ≥ `min_wal_number` contain
   exactly the unflushed data. (Filtering by the manifest's
   `last_sequence` would be wrong — that counter can include writes
   that were only ever in WAL + memtable.)
   A torn tail is then **durably truncated** (`ftruncate` + fsync) to
   the last valid record boundary. Without this, the chained-crash
   harness found a real protocol hole: crash → recovery stops at the
   tear and opens a new WAL → the torn bytes are now mid-sequence → the
   next recovery must either silently skip them (unsound: a mid-sequence
   tear is indistinguishable from real corruption) or refuse to open.
   Truncation restores the invariant "a tear can only be the final
   record ever written", so a torn record in a *non-final* WAL is
   always reported as corruption rather than guessed at.
4. `last_sequence = max(manifest.last_sequence, max replayed seq)`;
   `next_file_number = max(manifest.next_file_number, 1 + max file
   number seen on disk)` — orphans minted after the last MANIFEST write
   may exceed the recorded counter.

**Ordering invariants that make this safe** (each arrow is a completed,
durable step before the next begins):

```
write SST → fsync(SST) → fsync(dir)            // SST + its dirent durable
  → write MANIFEST.tmp → fsync(MANIFEST.tmp)
  → rename(MANIFEST.tmp, MANIFEST) → fsync(dir) // new version durable
  → unlink obsolete WALs / SSTs                 // only now
```

- An SSTable **and its directory entry** are durable before the
  MANIFEST that references it is renamed (no dangling references).
- The MANIFEST rename that drops a WAL from `min_wal_number` is durable
  **before** that WAL is unlinked (no acked write ever exists in zero
  durable places).
- Compaction inputs are unlinked only **after** the MANIFEST that
  removes them is durable, and only when no live version/iterator still
  references them (live-set check over all referenced versions plus
  in-flight compaction outputs).
- The ack for a write happens after `write()` (and per-policy sync) of
  its WAL record — an acked write is always in {WAL} or {WAL, SST} or
  {SST}, never in nothing.
- SSTables and the MANIFEST are **always** fsync'd; `FsyncPolicy`
  governs the WAL only.
- Any *real* (non-injected) write/fsync failure in the background
  latches a background error and the DB refuses further writes — fsync
  is never retried after failure (post-failure fsync semantics are
  undefined).

---

## 2. In-memory structures

### 2.1 Memtable

An arena-backed concurrent skiplist (LevelDB-style): immutable nodes,
atomic forward pointers with release/acquire ordering — one writer
(serialized by the commit path) and many lock-free readers. Entries are
encoded inline in the arena:

```
varint32 internal_key_len | user_key | tag(8B) | varint32 value_len | value
```

Max height 12, branching 1/4, per-node height chosen from a per-memtable
RNG. The arena hands out 4 KiB chunks and is freed as a unit when the
last reference to the memtable drops (`std::shared_ptr<MemTable>` held
by the DB, by in-flight reads, and by iterators — the arena lifetime
problem solved by construction).

When a memtable reaches `write_buffer_size` it is atomically swapped
for a fresh one (together with a new WAL segment — WAL rotation is
coupled to memtable rotation, which is what makes replay-all-WALs
correct) and pushed onto the immutable queue. A dedicated **flush
thread** drains immutables to L0 oldest-first; a separate **compaction
thread** runs level compactions — so a long L1→L2 compaction can never
block flushes into stalling writers that L0 could still absorb.

### 2.2 Write path (group commit)

Writers enqueue onto a writer queue under the DB mutex. The queue head
becomes the *leader*: it coalesces all pending batches into one WAL
record group, releases the mutex, does the WAL append + per-policy
sync **once** for the group, re-acquires the mutex, applies all batches
to the memtable, assigns results, and wakes the group. This amortizes
fsync across concurrent writers — the difference between ~200 and many
thousands of synchronous commits/sec.

The global `last_sequence` is published (made visible to readers)
**after** the memtable apply completes, so a concurrent reader can
never observe half of a batch: readers snapshot the published sequence,
and unpublished entries are invisible by MVCC filtering.

The leader runs the WAL append with the DB mutex *released*; under
`fsync=interval` the background sync tick touches the same file
concurrently, so `WalWriter` serializes `add_record`/`sync` with its
own (uncontended) mutex. The 12k-kill-point matrix caught the variant
without it: a sync could flush a half-appended buffer and leave a
CRC-invalid record mid-WAL — a one-in-thousands interleaving.

Backpressure (writes stall rather than OOM), checked by the leader
before committing:
- L0 file count ≥ `l0_slowdown_trigger` (8): sleep 1 ms per batch.
- L0 file count ≥ `l0_stop_trigger` (12), or immutable queue full
  (≥ 2): block on a condvar until the background thread catches up.

### 2.3 Versions

A `Version` is an immutable snapshot of the file-DAG: `files[level]`
sorted vectors of `FileMeta{number, size, smallest, largest}`. The
current version is a `std::shared_ptr<Version>` swapped under the DB
mutex; reads and iterators grab a reference and are immune to
concurrent flush/compaction. Obsolete files are deleted when the last
version referencing them dies (checked against the live-version set
after each version install).

---

## 3. Read path

`Get(key, snapshot_seq)` constructs lookup key `key ⧺ tag(snapshot_seq,
MaxType)` and probes, in order, stopping at the first hit:

1. active memtable,
2. immutable memtables (newest first),
3. L0 files **newest-file-number first** (L0 files may overlap),
4. levels 1..N: binary search for the single file whose
   `[smallest, largest]` may contain the key.

Every SSTable probe checks the Bloom filter first (skips ~99% of
files that don't contain the key), then binary-searches the index
block, then one data block. A `Delete` hit returns NotFound.

An optional sharded LRU **block cache** (`block_cache_bytes`, default
64 MiB) caches uncompressed data blocks keyed by (file number, offset);
without it every read pays a pread + CRC + parse even when hot. The
benchmark reports strata with and without it.

Range scans: a heap-based merging iterator over (memtable, immutables,
every L0 file, one concatenating iterator per level ≥ 1), wrapped by a
`DBIter` that enforces MVCC visibility — for each user key, take the
first version with `seq <= snapshot`, suppress older versions and
everything under a tombstone. Forward iteration only in v1 (documented
limitation; YCSB A/B/C does not scan).

---

## 4. MVCC

- Global atomic `last_sequence`; each committed op gets the next
  sequence. Readers without an explicit snapshot read at
  `last_sequence` as-of entry.
- `GetSnapshot()` pins a sequence in a doubly-linked snapshot list;
  `ReleaseSnapshot` unpins. Point reads and iterators evaluate
  visibility as "newest version with `seq <= snapshot_seq`".
- Compaction GC (LevelDB's `last_sequence_for_key` rule): walking
  merged input in internal-key order, a version of user key K is
  dropped iff a newer version of K has already been emitted *at or
  below the same snapshot boundary* — i.e. versions still visible to
  some live snapshot are retained. A tombstone is dropped (not just its
  victims) only when compacting into the bottommost level for its key
  range and no snapshot can still observe it.

---

## 5. Compaction policy

**Shape:** size-tiered L0 (overlapping files, one per memtable flush) +
leveled L1..L6 (non-overlapping files, ~8 MiB each), target sizes
`target(L1) = 64 MiB`, `target(Ln) = 10 × target(Ln-1)`.

**Picking** (one background thread; highest score ≥ 1.0 wins):

```
score(L0) = file_count / 4
score(Ln) = level_bytes / target(Ln)
```

- L0→L1: inputs = **all** L0 files (they overlap) + all overlapping L1
  files. This is the size-tiered step: L0 absorbs bursts, and merging
  all of L0 at once bounds read amplification.
- Ln→Ln+1 (n ≥ 1): round-robin cursor picks the next file after the
  last compacted key; inputs = that file + all overlapping Ln+1 files,
  then inputs are **expanded to a fixpoint** so that no user key spans
  an input-set boundary (the classic LevelDB boundary-key bug: two Ln
  files may share a boundary user key at different sequences; compacting
  one without the other can surface a stale version).
- Output files are cut at ~8 MiB, but **never between two entries with
  the same user key** (preserves the level invariant above).

**Write amplification.** Leveling with fanout F = 10 rewrites each byte
~F/2 times per level it descends. For a DB spanning L1..Ln:

```
WA ≈ 1 (WAL) + 1 (flush) + Σ_{i=1..n-1} (1 + F/2·overlap_factor)
```

worst case ≈ `1 + 1 + 11·(n-1)`; measured in practice well below that
because overlap is partial. The benchmark reports **measured** WA =
(wal_bytes + flush_bytes + compaction_write_bytes) / user_bytes from
internal counters, alongside RocksDB's from its statistics — the design
target is that strata's WA is within ~1.5× of RocksDB's on the YCSB
load phase, and the gap is explained rather than hidden.

Trade-off notes (why leveled + tiered L0): pure size-tiered minimizes
WA but lets space and read amplification balloon; pure leveled from L0
would force a compaction per flush. Tiered-at-L0 + leveled-below is the
standard compromise (LevelDB, RocksDB default) and the right default
for point-lookup-heavy workloads like YCSB B/C.

---

## 6. Durability model & crash harness

Claim to prove: **no acknowledged write is ever lost across SIGKILL at
any instruction boundary, under any fsync policy; and recovery never
accepts a torn or corrupt record.** (Power-loss durability additionally
requires `fsync=always`; with `use_fullfsync` on macOS it extends
through the drive cache. SIGKILL cannot test power loss — the page
cache survives — so the matrix claims exactly what it tests.)

Two kill mechanisms, both real SIGKILL:

1. **Byte-offset kills** (deterministic torn writes): the Env write
   path counts every byte handed to `write(2)` across all files. With
   `STRATA_CRASH_AT_BYTES=n`, the write that crosses byte *n* is split —
   the prefix up to *n* is written, then `raise(SIGKILL)`. Sweeping *n*
   randomly places a torn write at arbitrary byte offsets inside WAL
   records, SSTable blocks, MANIFEST.tmp — everywhere.
2. **Timer kills**: the orchestrator SIGKILLs the child at a random
   wall-clock offset, landing kills between syscalls, mid-compaction,
   mid-flush.

Protocol per kill point: orchestrator forks a workload child. The child
writes batches; after each `DB::Write` returns it writes an ack line to
an inherited pipe with a raw `write(2)` (no userspace buffering — an
acked line implies the DB write returned). Values embed
`crc32c(key ⧺ op_index)` so any accepted-but-torn value is detectable.
After the kill, the orchestrator reopens the DB and asserts:

- **A (durability):** every acked key is readable with the correct value;
- **B (no torn accept):** a full scan finds only values whose embedded
  checksum verifies;
- **C (prefix consistency):** the set of recovered ops is a prefix of
  the child's intent order (single-writer child ⇒ recovery keeps
  exactly ops with `seq ≤ S` for some `S ≥` last acked seq).

Matrix dimensions: {fsync always, interval, never} × {byte-offset kill,
timer kill} × {fresh DB, chained crash→recover→continue on the same
directory}. Chained runs use a tiny `write_buffer_size` so kills land
during flush and compaction, not just WAL appends. ≥10,000 kill points
total, parallelized across worker slots; CI runs the same harness.

---

## 7. Fuzzing

`fuzz_wal`, `fuzz_sstable`, and `fuzz_manifest` (every parser on the
recovery path gets a fuzzer) are libFuzzer targets built with
`-fsanitize=fuzzer,address,undefined`: bytes in → parser must either
parse or reject, never crash, overflow, over-read, or spin. Hardened
surfaces: varint termination, `length` vs remaining-file bounds,
restart-array count vs block size, BlockHandle offsets vs file size,
num_probes = 0, shared_len exceeding previous key. Seed corpora are
real files generated by the unit tests. CI runs each target for a
bounded window; local runs go longer.

## 8. Benchmarks

YCSB A (50/50 read/update), B (95/5), C (read-only), zipfian θ=0.99,
against RocksDB via a common `EngineIface`. Both engines: compression
off, WAL on, same value size, same key space, same machine, defaults
otherwise (documented). Reported: throughput, p50/p99 latency, measured
write amplification, and an explicit **"where strata loses"** analysis.
Losses are expected on read-heavy workloads (RocksDB's block cache +
per-block filters), high-concurrency writes (pipelined WAL), and space
(no compression) — the point of the table is that the numbers are real.

## 9. Explicit non-goals (v1)

Reverse iteration; block compression (snappy/zstd); log-structured
MANIFEST; per-block Bloom filters / partitioned indexes; column
families; transactions beyond atomic WriteBatch; TTL. Each is listed
with its RocksDB analogue in the README so the scope cut is visibly a
choice, not ignorance.
