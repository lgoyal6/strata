#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

class Env;
class Snapshot;

// When the WAL is synced relative to the write acknowledgement. All three
// policies survive SIGKILL (the page cache outlives the process); they differ
// only in the power-loss window. See docs/DESIGN.md §1.2 and §6.
enum class FsyncPolicy : std::uint8_t {
    kAlways,   // fsync (or F_FULLFSYNC) before every ack
    kInterval, // background sync every wal_sync_interval_ms; ack does not wait
    kNever,    // kernel writeback only
};

struct Options {
    bool create_if_missing = true;
    bool error_if_exists = false;

    // --- write path ---
    std::size_t write_buffer_size = 8u << 20; // memtable size before flush
    int max_immutable_memtables = 2;          // writers block above this
    FsyncPolicy fsync_policy = FsyncPolicy::kAlways;
    int wal_sync_interval_ms = 5; // used by FsyncPolicy::kInterval
    bool use_fullfsync = false;   // macOS: F_FULLFSYNC (through the drive cache)

    // --- compaction ---
    int l0_compaction_trigger = 4; // L0 files before L0->L1 compaction
    int l0_slowdown_trigger = 8;   // 1ms sleep per write above this
    int l0_stop_trigger = 12;      // writes block above this
    std::uint64_t target_file_size = 8u << 20;
    std::uint64_t l1_target_bytes = 64ull << 20;
    double level_size_multiplier = 10.0;

    // --- tables ---
    std::size_t block_size = 4096; // uncompressed payload target per block
    int block_restart_interval = 16;
    int bloom_bits_per_key = 10;               // 0 disables the filter
    std::size_t block_cache_bytes = 64u << 20; // 0 disables the block cache

    Env* env = nullptr; // nullptr -> Env::default_env()
};

struct ReadOptions {
    const Snapshot* snapshot = nullptr; // nullptr -> read at latest sequence
    bool fill_cache = true;
};

struct WriteOptions {};

} // namespace strata
