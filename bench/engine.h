#pragma once

#include <memory>
#include <string>

// Minimal engine facade so strata and RocksDB run the identical workload
// loop. Fairness rules (docs/BENCHMARKS.md): compression off in both, same
// write buffer / block cache / bloom bits / L0 triggers / file sizes, WAL on,
// sync mode matched per run.
struct BenchEngine {
    virtual ~BenchEngine() = default;
    // full_fsync: on macOS, sync writes push through the drive cache
    // (F_FULLFSYNC) — RocksDB always does this for sync=true there, so the
    // apples-to-apples sync comparison needs strata in the same mode.
    virtual bool open(const std::string& dir, bool sync_writes, bool full_fsync,
                      std::string* err) = 0;
    virtual bool put(const std::string& key, const std::string& value) = 0;
    // *found = false on a clean miss; returns false only on error.
    virtual bool get(const std::string& key, std::string* value, bool* found) = 0;
    // Seek to start_key, iterate up to n entries. Returns entries touched
    // or -1 on error.
    virtual int scan(const std::string& start_key, int n) = 0;
    virtual std::string stats_summary() = 0; // human-readable, incl. write amp
    virtual void close() = 0;
};

std::unique_ptr<BenchEngine> make_strata_engine();
std::unique_ptr<BenchEngine> make_rocksdb_engine(); // nullptr if not compiled in
