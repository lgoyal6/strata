# strata — interview rehearsal

Answers rehearsed as spoken: **claim → mechanism → number**. Every factual claim
cites `file:line` in this repo. Anything the code does not support is flagged as
an open question, not smoothed over. Line numbers verified against the working
tree on 2026-07-28 (single commit `9ddaa12`).

---

## 1. What fires a compaction, and how backpressure hangs off the same numbers

**Claim.** There are two separate mechanisms that get conflated: the *flush*
trigger (memtable size) and the *compaction* trigger (a score function over the
level shape). Backpressure is keyed off the same L0 file count the compaction
score uses, at three escalating thresholds: 4 / 8 / 12.

**Mechanism — flush side.** A write leader calls `make_room_for_write`
(`src/db/db_impl.cc:362-400`). If the active memtable exceeds
`write_buffer_size` (8 MiB default, `include/strata/options.h:25`), it calls
`rotate_memtable_and_wal` (`src/db/db_impl.cc:395-398`, body at `402-420`):
new WAL segment, memtable pushed onto `imms_`, fresh memtable stamped with the
new WAL number (`db_impl.cc:414-417`), then `bg_work_cv_.notify_all()`
(`db_impl.cc:418`). A dedicated flush thread drains `imms_` oldest-first
(`db_impl.cc:518-535`) and each completed flush adds one L0 file.

**Mechanism — compaction side.** A second dedicated thread
(`compaction_thread_main`, `db_impl.cc:668-702`) sleeps on `bg_work_cv_` and,
when woken (by rotation `418`, by a completed flush `532`, or by its own loop
re-scoring after each job), asks `VersionSet::pick_compaction`
(`src/db/version.cc:541-569`) for work:

- `score(L0) = L0_file_count / l0_compaction_trigger` with trigger = 4
  (`version.cc:546-550`, `options.h:32`). Note the comparison is `>=` against
  a best-score initialized to 1.0, so **exactly 4 L0 files fires it**.
- `score(Ln) = level_bytes / target_bytes(n)` for n ≥ 1, strictly `> 1.0`
  (`version.cc:552-562`), with `target(L1) = 64 MiB` and ×10 per level
  (`version.cc:532-539`, `options.h:36-37`).
- Highest score wins; L0→L1 takes *all* L0 files as inputs since they overlap
  (`version.cc:645-647`); deeper levels use a round-robin cursor plus
  boundary-key expansion (`version.cc:649-661`, the LevelDB boundary bug fix
  at `587-621`).

**Mechanism — backpressure interaction.** The write leader checks, in order,
inside `make_room_for_write`:

1. **Soft slowdown**: `l0_files >= l0_slowdown_trigger (8)` and below stop →
   release the lock, sleep exactly 1 ms, **once per batch** (an `allow_delay`
   flag prevents repeat sleeps — `db_impl.cc:363, 369-378`). The point is to
   donate a timeslice to the compaction thread without ever stalling a write
   indefinitely at this tier.
2. If the memtable has room, return — writes proceed (`db_impl.cc:379-381`).
3. **Hard stall A**: immutable queue full (`imms_.size() >=
   max_immutable_memtables`, default 2, `options.h:26`) → block on `stall_cv_`
   (`db_impl.cc:382-387`).
4. **Hard stall B**: `l0_files >= l0_stop_trigger (12)` → block on `stall_cv_`
   (`db_impl.cc:389-393`).
5. Otherwise rotate and loop.

Stalled writers are woken by the flush thread (`db_impl.cc:531`) and the
compaction thread (`db_impl.cc:699`) after every completed job. The option
sanitizer enforces `compact ≤ slowdown ≤ stop` so the tiers can't invert
(`db_impl.cc:37-39`). Stall time is measured and exported
(`stall_micros`, `db_impl.cc:374, 386, 392`).

**Numbers.** Defaults 4 / 8 / 12 (`options.h:32-34`). In the committed YCSB
runs the backpressure never engaged: `stall_ms=0` on every strata row
(`bench/results/ycsb.txt:4,9,29`) — compaction kept up at this write rate, so
the honest statement is "the mechanism exists and is tested, but the bench
never drove it into a stall."

---

## 2. Group commit, end to end

**Claim.** "Leader election" is just FIFO queue order — the front of a deque
becomes the leader; there is no election protocol. The leader does WAL append,
per-policy fsync, and the memtable apply for the *whole group* while every
other writer in the group sleeps on its own condvar.

**Mechanism, in commit order:**

1. **Enqueue + wait.** Every writer wraps its batch in a stack-allocated
   `Writer` with a personal `condition_variable` (`src/db/db_impl.h:49-55`),
   pushes it onto `writers_`, and waits until either `done` or it is at the
   front (`db_impl.cc:252-257`). Wake with `done == true` → the batch was
   committed *by someone else*; return that leader's status having never
   touched the WAL or memtable (`db_impl.cc:258-260`).
2. **Leader builds the group.** The front writer runs backpressure (§1), then
   `build_batch_group` (`db_impl.cc:331-360`). Batching rule: walk the queue
   in FIFO order, appending batches until the group would exceed `max_size` =
   1 MiB — reduced to `lead_size + 128 KiB` when the lead batch is ≤ 128 KiB,
   so a small write is never held hostage to a megabyte of coalescing
   (`db_impl.cc:336-340`). First follower triggers a copy into the leader-owned
   scratch `tmp_batch_` (`db_impl.cc:350-355`, `db_impl.h:111`). The walk
   stops at the first batch that would overflow — no reordering, ack order =
   sequence order.
3. **Sequencing.** Group gets `first_seq = last_sequence + 1`; ops are
   contiguous (`db_impl.cc:267-269`).
4. **I/O with the mutex released** (`db_impl.cc:275`): `wal_->add_record`
   appends header+payload and flushes the userspace buffer to the kernel with
   `write(2)` *before any ack* (`src/wal/wal_writer.cc:41-49` — this is what
   makes fsync=never still SIGKILL-durable). Then `sync_wal_for_policy`
   (`db_impl.cc:324-329`): `kAlways` → `wal_->sync()` = `fsync`, or
   `F_FULLFSYNC` on macOS when `use_fullfsync` (`src/util/env.cc:172-191`);
   `kInterval` → nothing here, a background thread syncs every 5 ms
   (`db_impl.cc:910-928`, `options.h:28`); `kNever` → nothing.
   **The fsync happens exactly once per group, by the leader, before the
   memtable apply and before any waiter is released.**
5. **Memtable apply — leader only** (`db_impl.cc:283`). Followers never insert
   their own batches. This is single-writer by construction (the skiplist
   needs exactly one writer; readers are lock-free, `src/db/skiplist.h:6,
   156-159`).
6. **Publish after apply.** Re-lock, then `set_last_sequence(last_seq)`
   (`db_impl.cc:287-292`). Readers snapshot the published sequence, so a
   concurrent reader can never observe half a batch — visibility is MVCC
   filtering, and unpublished sequences are invisible.
7. **Failure latch.** Any WAL/sync error → `record_background_error`
   (`db_impl.cc:296-300`, `930-937`): the DB refuses all future writes rather
   than risk a diverging on-disk prefix; every writer in the group gets the
   same error status.
8. **Wake protocol.** Leader pops the queue through `last_writer`, marks each
   `done` with the shared status, notifies each personal condvar, then
   notifies the new front — which becomes the next leader
   (`db_impl.cc:306-320`).

One subtlety worth volunteering: under `kInterval` the background sync thread
touches the same file as a concurrent leader append, so `WalWriter` serializes
`add_record`/`sync` with its own uncontended mutex (`wal_writer.cc:32`). The
crash matrix caught the variant without it — a sync flushing a half-appended
buffer left a CRC-invalid record mid-WAL (README.md:71 says kill point
~11,700 of 12,000; **the buggy variant itself is not in git history — single
squashed commit — so that story is documented, not diffable**).

**Numbers.** Group commit's one job is amortizing the sync: at
`sync=1` with `F_FULLFSYNC`, 4 threads, 50k ops — strata 1,015 ops/s vs
RocksDB 20 ops/s (`bench/results/ycsb.txt:575-576` vs `533-534`); both pay
~4 ms per drive-cache flush, the difference is commits-per-flush
(`docs/BENCHMARKS.md:86-91`). With plain `fsync` (macOS: doesn't flush the
drive cache) strata does 85,011 ops/s (`ycsb.txt:52-53`).

---

## 3. The recovery truncation fix (torn WAL tail)

**Claim.** The bug class: recovery that *stops* at a torn WAL tail but leaves
the torn bytes on disk converts a benign tear into what looks like
mid-sequence corruption after the *next* crash. The fix is to durably truncate
the tear at recovery time, which restores the invariant "a tear can only ever
be the final record of the final WAL" — and that invariant is what lets a tear
anywhere else be treated as real corruption.

**Mechanism — detection.** `WalReader::read_record` accepts a record iff the
9-byte header is complete, `length` fits in the remaining file (bounds-checked
*before* allocation — a torn length field can decode huge,
`src/wal/wal_reader.cc:90-94`), and the CRC over type+payload matches
(`wal_reader.cc:102-109`). Any failure sets `truncated_tail_` and stops;
`valid_offset_` advances only past fully CRC-valid records
(`wal_reader.cc:110`).

**Mechanism — the old behavior and why chained crashes exposed it.** (Old code
is **not in git** — one squashed commit — the narrative is documented in
`docs/DESIGN.md:246-254` and the invariant comment at
`src/db/db_impl.cc:188-191`.) Old flow: crash tears the last record of WAL *N*
→ recovery replays to the tear, stops, opens fresh WAL *N+1*, keeps running.
The torn bytes are still sitting in WAL *N*, which is now a **non-final** WAL.
Second crash → second recovery replays all WALs ≥ `min_wal_number` and hits a
CRC failure in the *middle* of the sequence. At that point it cannot
distinguish "stale tear from last time" from "real corruption in the middle of
a log": skipping is unsound (you'd skip genuinely corrupt data and everything
after it), refusing to open is an availability loss for a benign cause. The
single-crash matrix never sees this; only `mode=chain` (same directory across
kills, `tools/crash_test.cc:23-24`) does.

**Mechanism — the fix.** After replay, a tear is tolerated only in the final
WAL (`db_impl.cc:192-195` — a torn record in a non-final WAL returns
`Status::corruption`). The file is then cut at `valid_offset()` via
`env_->truncate_file` (`db_impl.cc:198-204`), which is `ftruncate` **plus
fsync** (`src/util/env.cc:295-303`, contract at `env.h:70-73`). Rotation only
ever happens after a completed record (`db_impl.cc:188-191`), and WAL files
are never recycled (monotonic numbers, `docs/DESIGN.md:36-37`), so after
truncation the invariant holds again.

Why *durable* truncation: for SIGKILL alone, plain `ftruncate` suffices —
syscall effects survive process death. The fsync closes the power-loss
variant, where losing the truncate's metadata would resurrect the torn bytes.
Honest footnote: the harness verifies the SIGKILL case only (§5).

**Numbers.** The chain-mode rows that exercise this:
`bench/results/crash_matrix.txt:7-12` — 6 configs × 1000 iterations, 5,753
real kills, 0 failures across ~1.08 M acked writes verified in chain mode.

---

## 4. Why replay-all-WALs has no sequence filter

**Claim.** There is nothing valid to filter *against*, and the file set is
already the filter: WAL rotation is coupled to memtable rotation, and
`min_wal_number` only advances when a flush's MANIFEST write is durable — so
"every record of every WAL ≥ `min_wal_number`" is *exactly* the unflushed
suffix. Filtering by the persisted `last_sequence` would silently drop
acknowledged writes.

**The reasoning, from code:**

1. **Coupling.** `rotate_memtable_and_wal` creates a new WAL in the same
   critical section that retires the memtable (`src/db/db_impl.cc:402-420`);
   each memtable records the WAL it drains from (`db_impl.cc:416`, and at
   recovery `152-153, 221`). One WAL ↔ one memtable's worth of data.
2. **Retirement is durable-first.** When the flush thread retires a memtable,
   the same `VersionEdit` that adds the L0 file sets `min_wal_number` to the
   next unflushed memtable's WAL (`db_impl.cc:549-557`), and
   `log_and_apply` → `write_manifest` fsyncs the MANIFEST via
   tmp + fsync + rename + dir-fsync *before* the edit takes effect
   (`src/db/version.cc:488-529`, `464-485`). Obsolete WALs are unlinked only
   after (`db_impl.cc:533`, `950-963`). So a WAL is dropped from the replay
   set if and only if its contents are durably in SSTs.
3. **Why a sequence filter is actively wrong.** The MANIFEST's
   `last_sequence` is written as the *live global* sequence counter —
   `write_manifest` stores `last_sequence_.load()` (`version.cc:449`), and
   that counter is bumped on every commit (`db_impl.cc:290`), including writes
   that exist only in WAL + memtable. So "skip records with seq ≤
   manifest.last_sequence, they're already durable" is false — it would drop
   acked data. (Note: `docs/DESIGN.md:216` annotates the field as "max seq
   persisted in SSTs" — **the code does not implement that**; the recovery
   note at `DESIGN.md:243-245` states the correct reasoning. Know this
   discrepancy before someone quotes the doc at you.)
4. **Idempotence makes the no-filter choice safe under re-replay.** If a crash
   lands *during* recovery after some replay-flushed L0s were installed but
   before old WALs are retired (recovery deliberately does not advance
   `min_wal` on mid-replay flushes — `db_impl.cc:166-171`; it advances only
   after full replay, `222-229`), the next recovery re-applies the same
   records. Duplicates are byte-identical internal keys (same user key, same
   sequence, same value). Reads resolve them newest-copy-first: L0 is sorted
   by file number = flush order = sequence order (`version.cc:509-512`) and
   probed newest-first (`version.cc:165-167`, `308-310`); compaction drops
   exact-duplicate internal keys, keeping the first (`db_impl.cc:825-829` —
   the comment says outright these can only come from re-replayed WALs).
5. **Sequence restoration** needs no filter either: recovery tracks
   `max(manifest last_sequence, max replayed batch seq)` (`db_impl.cc:131,
   159-160, 208`).

So the invariant to say out loud: **the WAL file-number floor is the sequence
filter** — coarse-grained, durable, and maintained by the flush protocol; a
per-record filter would re-derive it from a counter that means something else.

---

## 5. Durability taxonomy — what 11,149 SIGKILLs do and do not prove

**Claim.** Four failure modes form a strict ladder, and the crash matrix
verifies exactly the first rung: process death. Claiming the others from a
SIGKILL harness would be dishonest, and the design docs say so explicitly
(`docs/DESIGN.md:454-459`, README.md:51-55).

**The ladder, with mechanism:**

| survives | what's lost | what strata relies on | verified? |
|---|---|---|---|
| **process crash (SIGKILL)** | userspace state | acked ⇒ bytes reached the kernel: `add_record` ends with `file_->flush()` = `write(2)` before any ack (`src/wal/wal_writer.cc:45-49`, `src/util/env.cc:160-170`). Page cache outlives the process ⇒ **all three fsync policies** must show zero loss | **yes — 12,000 iterations, 11,149 real SIGKILLs, 2,223,252 acked writes verified, 0 failures** (`bench/results/crash_matrix.txt:1-12`; kills column sums to 11,149) |
| **kernel panic** | page cache | only what was fsync'd (to the drive or its cache): `always` survives; `interval` loses ≤ one 5 ms window of acks (`options.h:28`); `never` unbounded. MANIFEST and SSTs are *always* fsync'd regardless of WAL policy (`version.cc:464-485`, `db_impl.cc:609-618`) | **no** — SIGKILL cannot kill the kernel |
| **power loss** | drive write cache | macOS `fsync` does **not** flush the drive cache; `F_FULLFSYNC` does, and it's gated on `use_fullfsync`, default **false** (`env.cc:177-190`, `options.h:29`). So even `fsync=always` at defaults does not claim power-loss durability on macOS | **no** |
| **disk lying about flush** | whatever firmware dropped | nothing — no software in this repo can defend against or detect it | **no, and not testable in software alone** |

**The kill harness, so the 11,149 number is defensible in detail:** two
mechanisms, both real `SIGKILL` — byte-offset kills via an Env choke point
that counts every byte handed to `write(2)` and splits the write that crosses
`STRATA_CRASH_AT_BYTES`, writing the prefix then `raise(SIGKILL)`
(`src/util/env.cc:30-74`); and wall-clock timer kills. After each kill the
orchestrator reopens the DB and asserts three properties: every acked op
survives with the right value, every recovered value passes its embedded
checksum, and the recovered state is exactly the acked prefix (± the single
in-flight op) — nothing torn, resurrected, or reordered
(`tools/crash_test.cc:16-24`). Acks go through an `O_APPEND` file written with
raw `write(2)` after `DB::write` returns (`crash_test.cc:8-13`; note
`DESIGN.md:475` says "pipe" — the code uses a file, and its comment explains
why). 12 configs = {always, interval, never} × {bytes, timer} × {fresh,
chain}, 1000 iterations each; 851 iterations exited cleanly before the kill
landed, hence 11,149 real kills out of 12,000.

**What I'd need to claim more:**

- *Kernel panic / power loss (protocol level):* extend the existing Env fault
  layer to simulate loss above the fsync line — on simulated crash, discard
  all bytes written since the last successful `sync()` per file, with
  sector-granularity truncation/reordering — and rerun the same A/B/C matrix.
  On Linux, `dm-log-writes` replays real block traces to the same effect.
  This verifies the *ordering protocol* (fsync placement), which is the part
  strata controls.
- *Power loss (physical) / lying disk:* a power-cut rig (or SQLite-style
  torture testing) against the specific hardware; no pure-software test can
  establish it. The precise sentence for the resume claim: "zero acked-write
  loss across 11,149 SIGKILLs under all three fsync policies; power-loss
  durability is an fsync=always + F_FULLFSYNC *design* claim, tested only to
  the syscall boundary."

---

## 6. Root cause: 0.65–0.79× reads at 4 threads, 0.43× scans

**The numbers first** (all from `bench/results/ycsb.txt`; ratios =
strata / RocksDB ops/s):

| workload | 1 thread | 4 threads |
|---|---|---|
| load (insert) | **1.24×** (438,529 / 353,679 — lines 3, 58) | 0.72× (292,968 / 404,424 — lines 28, 296) |
| A 50/50 | **1.17×** (495,959 / 425,261 — 8, 106) | **0.65×** (449,064 / 694,836 — 33, 344) |
| B 95/5 | **1.11×** (694,585 / 628,504 — 13, 155) | **0.79×** (1,548,368 / 1,949,827 — 38, 393) |
| C read-only | **1.04×** (798,724 / 764,631 — 18, 203) | **0.69×** (1,831,395 / 2,666,157 — 43, 441) |
| E scan | **0.43×** (33,907 / 79,098 — 23, 250) | 0.47× (130,463 / 275,785 — 48, 488) |

Two different diseases: the point-read loss appears **only at 4 threads**
(strata wins every non-scan workload single-threaded) — that's contention.
The scan loss exists **at one thread** — that's per-op CPU/layout, not
contention. Treat them separately or the answer is wrong.

### 6a. Point reads: the read path serializes on shared cache lines

Scaling t1→t4 on pure reads (C): strata 2.29× vs RocksDB 3.49×. Per `get`,
strata touches this shared state:

1. **The global DB mutex, once per get** — `DBImpl::get` locks `mutex_` to
   capture `{seq, mem_, imms_, version}` (`src/db/db_impl.cc:431-440`). The
   critical section is short but the lock word itself ping-pongs between
   cores at ~1.8 M ops/s.
2. **A second global mutex inside the first** — `versions_->current()` takes
   `VersionSet::mu_` (`src/db/version.cc:335-338`).
3. **shared_ptr refcount traffic on shared control blocks** — copying `mem_`
   and `version` is 4 atomic RMWs (2 acquire + 2 release) on *the same two
   control blocks for every thread* (`db_impl.cc:437-439`). This is the
   textbook layout problem RocksDB's thread-local SuperVersion exists to
   avoid: same work, but the refcounts live on per-thread cache lines.
4. **A third global mutex per file probe** — every candidate SST goes through
   `TableCache::find_table`, `std::lock_guard` on a single `mu_`
   (`src/db/table_cache.cc:5-14`), *before* the bloom filter can reject
   (`version.cc:128-137` calls `find_table` first; the bloom check is inside
   `TableReader::get`, `src/table/table_reader.cc:107-117`). Zipfian C does
   this ~1–4× per get.
5. **Shared atomic counters on the hot path** — `bloom_checks`/`bloom_skips`
   per probe (`table_reader.cc:108-114`), cache `hits_`/`misses_` per block
   lookup (`src/util/cache.cc:16, 20`). Relaxed ordering doesn't prevent the
   cache-line ping-pong.
6. **Block cache: sharded but hit-mutating.** 16 shards, Fibonacci-hashed
   (`src/util/cache.h:40, 55-58`) — so "single-mutex cache" is *not* the
   story — but every hit takes the shard mutex and splices the LRU list
   (`cache.cc:13-19`). Under zipfian θ=0.99 the hottest block concentrates on
   one shard, and hits are writes. RocksDB's LRU also locks on hit, but its
   read path reaches the cache without the three global mutexes above.

At 1 thread every one of these is an uncontended ~20 ns operation — which is
why strata *wins* single-threaded (shorter code path than RocksDB: no column
families, no version chains — `docs/BENCHMARKS.md:113-116`). At 4 threads
they're all cross-core traffic. The fix path is known and deliberately out of
v1: per-thread/epoch-pinned source capture (SuperVersion-equivalent), bloom
check before table-cache lookup, per-shard or striped table cache, and
counter sharding (`BENCHMARKS.md:97-101`).

For A/B/load, add the write side: the group leader applies the *entire
group's* memtable inserts alone while followers sleep (`db_impl.cc:283`,
§2) — memtable apply is single-threaded by construction, and with `sync=0`
there's no fsync to amortize, so group commit is pure serialization +
condvar handoff. The tell in the data: strata's 4-thread load (292,968) is
*below its own* 1-thread load (438,529) — negative write scaling
(`ycsb.txt:28` vs `3`), while RocksDB gains (353,679 → 404,424).

### 6b. Scans: fixed cost is competitive, marginal per-row cost is ~15× worse

The latency shape localizes the problem: at 1 thread, **p50 is at parity
(12.3 µs vs 12.2 µs) but p95 is 181.0 µs vs 22.6 µs**
(`ycsb.txt:23` vs `250`). Short scans (dominated by the shared seek cost) tie;
long scans (up to 100 rows, `docs/BENCHMARKS.md:59`) blow up — so the loss is
in the per-row/per-block marginal path, ~(181−12)/100 ≈ 1.7 µs/row vs
RocksDB's ≈ 0.1 µs/row. Four mechanisms, in the code:

1. **Eager cursor-stack construction per scan op.** Every scan builds a fresh
   iterator: `new_iterator` captures sources under the DB mutex, then opens an
   iterator on the memtable, *every* L0 file (each a `TableCache::find_table`
   under its global mutex), and one `LevelIterator` per non-empty level
   (`db_impl.cc:455-485`, `version.cc:306-324`); each `TableReader::Iter`
   allocates its index-block iterator up front (`table_reader.cc:150-154`).
2. **Seek fans out to every child.** `MergingIterator::seek` seeks all k
   children (`src/table/merging_iterator.cc:25-30`); each child seek is an
   index binary search plus a data-block fetch through the shard-locked cache
   with two heap allocations (`table_reader.cc:169-176`, `205-223`) — even
   though only 1–2 children will contribute rows to a short scan.
3. **`next()` is a linear min-scan over all children.** Every row does
   `current_->next()` then `find_smallest()` — a full O(k) pass with an
   `InternalKeyComparator` compare per child, each behind a virtual call
   (`merging_iterator.cc:32-36, 61-70`). **`docs/DESIGN.md:380` claims a
   "heap-based merging iterator" — the code is a linear scan**; the comment
   at `merging_iterator.cc:59-60` argues k is small, but O(k) per row is
   exactly where a 100-row scan spends its time. RocksDB uses a specialized
   min-heap whose common case after `Next()` is a single comparison.
4. **Block boundaries re-enter the cache.** Every ~4 KiB (`options.h:40`,
   ~34 rows at this record size), `init_data_block` does a shard-locked cache
   lookup + LRU splice + `make_shared<Block>` + iterator allocation
   (`table_reader.cc:205-223`, `cache.cc:11-22`). There is no block pinning
   and no readahead; RocksDB pins the current block and iterates it raw.

**Open question (flagged, not smoothed):** the split between mechanism 3
(merge fan-out) and mechanisms 2/4 (block reload + allocations) is **not
profiled** — no flame graph is committed. The honest phrasing: "the code has
four identified mechanisms; ranking 3 over 4 is my expectation, not a
measurement. A `samply` run on workload E is the first thing I'd do next."
Also honest: DESIGN.md's own read-path section promised the heap that would
mitigate mechanism 3; the divergence is documented above so it can't be
sprung on you.

---

## Cross-checks worth having loaded

- Write amplification, same 1 M-record load: strata 4.53× vs RocksDB ~4.66×
  (`ycsb.txt:4`, `docs/BENCHMARKS.md:82-85`) — the LSM shape is fine; the
  losses are concurrency and iterator mechanics, not the storage format.
- "12,000" vs "11,149": 12,000 iterations, 851 clean exits before the kill
  landed, 11,149 real SIGKILLs (`crash_matrix.txt` kills column; README
  table). Say whichever you can defend — they're both true, but they're
  different numbers.
- Bench environment: M3 Pro, working set fits page cache — measures engine
  CPU paths, not disk (`BENCHMARKS.md:24-29, 125-127`). Both engines share
  that advantage.
