#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/slice.h"
#include "strata/status.h"
#include "strata/write_batch.h"

namespace strata {

// Opaque handle pinning a sequence number; obtained from DB::get_snapshot.
class Snapshot {
  protected:
    Snapshot() = default;
    ~Snapshot() = default;
};

// Point-in-time copies of the DB's internal counters (all monotonic).
struct DbStats {
    std::uint64_t user_bytes_written = 0; // key+value payload accepted
    std::uint64_t wal_bytes_written = 0;
    std::uint64_t flush_bytes_written = 0; // memtable -> L0
    std::uint64_t compaction_bytes_read = 0;
    std::uint64_t compaction_bytes_written = 0;
    std::uint64_t flush_count = 0;
    std::uint64_t compaction_count = 0;
    std::uint64_t write_stall_micros = 0;
    std::uint64_t bloom_checks = 0;
    std::uint64_t bloom_skips = 0; // file probes avoided by the filter
    std::uint64_t block_cache_hits = 0;
    std::uint64_t block_cache_misses = 0;

    // (wal + flush + compaction writes) / user bytes
    double write_amplification() const {
        const std::uint64_t disk =
            wal_bytes_written + flush_bytes_written + compaction_bytes_written;
        return user_bytes_written == 0
                   ? 0.0
                   : static_cast<double>(disk) / static_cast<double>(user_bytes_written);
    }
};

class DB {
  public:
    // On success *dbptr owns the database; caller deletes it to close.
    static Status open(const Options& options, const std::string& dbname, DB** dbptr);

    DB() = default;
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    virtual ~DB() = default;

    virtual Status put(const WriteOptions& opt, const Slice& key, const Slice& value) = 0;
    virtual Status remove(const WriteOptions& opt, const Slice& key) = 0;
    virtual Status write(const WriteOptions& opt, WriteBatch* updates) = 0;

    virtual Status get(const ReadOptions& opt, const Slice& key, std::string* value) = 0;
    virtual Iterator* new_iterator(const ReadOptions& opt) = 0;

    virtual const Snapshot* get_snapshot() = 0;
    virtual void release_snapshot(const Snapshot* snapshot) = 0;

    // Force the active memtable to L0 and wait for the flush to finish.
    virtual Status flush() = 0;
    // Compact everything down to the last level (test/bench hook).
    virtual Status compact_all() = 0;

    virtual DbStats stats() const = 0;
};

} // namespace strata
