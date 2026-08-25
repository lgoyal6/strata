# Contributing to strata

Thanks for looking. strata is a storage engine whose entire claim is a durability
contract, so the bar for changes near the write path is deliberately high and the
bar for everything else is normal.

## The contract you must not break

**Every acknowledged write survives a `kill -9`.** That is the one sentence the
whole repo exists to defend. `tools/crash_test.cc` tears `write(2)` calls at
randomized byte offsets and SIGKILLs the engine, then verifies that nothing
acknowledged was lost and no torn record was accepted. If a change of yours makes
that harness fail, the change is wrong, not the harness.

Corollaries worth stating, because they are easy to violate by accident:

- Nothing is externally visible before its WAL record is durable.
- Recovery may truncate a torn tail only where it can prove nobody was told the
  operation succeeded.
- A failed `fsync` is unrecoverable. The page cache is unknowable afterwards, so
  the engine fails outstanding waiters rather than retrying and lying.

## Getting oriented

| Path | What lives there |
|---|---|
| `include/strata/` | The public surface. `src/` implements it. |
| `docs/DESIGN.md` | Why each structure is shaped the way it is. Read before changing the WAL, SSTable or manifest formats. |
| `docs/BENCHMARKS.md` | Where the README's numbers come from. |
| `test/unit/` | Unit tests. `test/smoke.cc` is the quick end-to-end. |
| `tools/crash_test.cc` | The crash matrix. |
| `fuzz/` | libFuzzer targets for every recovery-path parser: WAL, SSTable, manifest. |
| `bench/` | YCSB harness, with a RocksDB baseline behind a flag. |
| `wasm/` | The browser demo build. |

## Building and testing

```bash
cmake --preset dev            # or dev-mac
cmake --build build
ctest --preset dev
```

Optional targets are off unless you ask for them:

```bash
cmake --preset dev -DSTRATA_BUILD_FUZZERS=ON    # clang only
cmake --preset dev -DSTRATA_BUILD_BENCH=ON      # needs RocksDB for the baseline
./fuzz/run_fuzz.sh
./bench/run_matrix.sh
```

Anything touching the WAL, recovery, compaction or the manifest should also run
the crash harness and the relevant fuzzer locally before you open a PR.

## What makes a good PR here

- One concern per PR, with a test that fails before and passes after.
- Format changes (WAL record, SSTable block, manifest) need a fuzz corpus entry
  and a note in `docs/DESIGN.md`, because they are the changes that turn into
  unreadable data on somebody's disk rather than into a failing test.
- New recovery paths need a crash-harness case, ideally driven through an existing
  failpoint rather than a new bespoke harness.
- Benchmark numbers in the README come from `bench/run_matrix.sh`. If your change
  moves them, include before and after output rather than editing the table.
- `clang-format` is pinned to 18 in CI. Run it before pushing; a format-drift
  failure is a wasted CI cycle for both of us.

## Good first areas

- The fuzz corpora are lopsided: 124 seeds for the WAL, 42 for SSTables, 13 for
  the manifest. Manifest seeds are the most useful thing you can add without
  knowing the engine.
- `bench/` only has a RocksDB baseline. A second baseline would make the numbers
  more legible.
- Portability: the engine targets POSIX. Reports from platforms other than Linux
  and macOS are welcome as issues even without a fix.

## Conduct

Be decent. Disagree about the code, not about the person.
