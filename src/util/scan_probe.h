#pragma once

// Opt-in scan-path counters. Compiled out entirely unless STRATA_SCAN_PROBE
// is defined (CMake: -DSTRATA_SCAN_PROBE=ON), so release builds carry no
// atomics on the iterator hot path. Used to attribute scan tail latency
// between iterator construction, block reads and version skipping.

#ifdef STRATA_SCAN_PROBE

#include <atomic>
#include <cstdint>

namespace strata {
namespace probe {

struct ScanCounters {
    std::atomic<std::uint64_t> scans{0};         // new_iterator() calls
    std::atomic<std::uint64_t> children{0};      // child iterators constructed
    std::atomic<std::uint64_t> l0_children{0};   // of which, one per L0 file
    std::atomic<std::uint64_t> block_reads{0};   // data-block fetches (cache hit or miss)
    std::atomic<std::uint64_t> block_misses{0};  // of which, actually read from disk
    std::atomic<std::uint64_t> internal_next{0}; // merging-iterator advances
    std::atomic<std::uint64_t> skipped{0};       // advances that yielded no user entry
    std::atomic<std::uint64_t> skipped_seq{0};   // ... because the entry is newer than the snapshot
    std::atomic<std::uint64_t> skipped_key{
        0}; // ... because it is a superseded version of a yielded key
    std::atomic<std::uint64_t> max_run{0};    // longest consecutive same-key skip run
    std::atomic<std::uint64_t> skip_seeks{0}; // targeted seeks that replaced a skip run
    std::atomic<std::uint64_t> compares{0};   // find_smallest key comparisons
};

ScanCounters& counters();

} // namespace probe
} // namespace strata

#define STRATA_PROBE_ADD(field, n)                                                                 \
    ::strata::probe::counters().field.fetch_add((n), std::memory_order_relaxed)

#else
#define STRATA_PROBE_ADD(field, n) ((void)0)
#endif
