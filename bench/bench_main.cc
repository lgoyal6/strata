// YCSB-style benchmark driver (docs/BENCHMARKS.md).
//
//   bench --engine strata|rocksdb --workload load|a|b|c|e
//         --dir D --records N --ops M --threads T --value-size B --sync 0|1
//         [--seed S]
//
// load: insert all N records (hashed key order).
// a: 50% read / 50% update       b: 95% read / 5% update
// c: 100% read                   e: 95% scan(<=100) / 5% insert
// Key selection: scrambled zipfian, theta 0.99 (YCSB default).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#endif

#include "bench/engine.h"
#include "bench/ycsb.h"

namespace {

const char* arg_value(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

// Peak resident set of this process. ru_maxrss is bytes on Darwin and kilobytes on
// Linux, which is a portability trap worth naming rather than a number worth guessing.
std::uint64_t peak_rss_bytes() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024ULL;
#endif
}

// Resident set right now, so a peak that a compaction spike produced can be told apart
// from a steady working set that stays resident.
std::uint64_t current_rss_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        return static_cast<std::uint64_t>(info.resident_size);
    }
    return 0;
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long total = 0, resident = 0;
    const int got = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    return got == 2 ? static_cast<std::uint64_t>(resident) * 4096ULL : 0;
#endif
}

// User + system CPU seconds charged to this process. This is the cost axis: wall time
// says how long you waited, CPU time says what you paid for, and on a machine shared
// with other work the two are not the same number.
double cpu_seconds() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec) +
           static_cast<double>(ru.ru_utime.tv_usec) / 1e6 +
           static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct WorkloadMix {
    int read_pct = 0;
    int update_pct = 0;
    int scan_pct = 0;
    int insert_pct = 0;
};

WorkloadMix mix_for(const std::string& w) {
    if (w == "a") {
        return {50, 50, 0, 0};
    }
    if (w == "b") {
        return {95, 5, 0, 0};
    }
    if (w == "c") {
        return {100, 0, 0, 0};
    }
    if (w == "e") {
        return {0, 0, 95, 5};
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    const std::string engine_name = arg_value(argc, argv, "--engine", "strata");
    const std::string workload = arg_value(argc, argv, "--workload", "load");
    const std::string dir = arg_value(argc, argv, "--dir", "/tmp/strata-bench");
    const std::uint64_t records =
        std::strtoull(arg_value(argc, argv, "--records", "1000000"), nullptr, 10);
    const std::uint64_t total_ops =
        std::strtoull(arg_value(argc, argv, "--ops", "1000000"), nullptr, 10);
    const unsigned threads =
        static_cast<unsigned>(std::strtoul(arg_value(argc, argv, "--threads", "1"), nullptr, 10));
    const std::size_t value_size = static_cast<std::size_t>(
        std::strtoul(arg_value(argc, argv, "--value-size", "100"), nullptr, 10));
    const bool sync_writes = std::strtol(arg_value(argc, argv, "--sync", "0"), nullptr, 10) != 0;
    const bool full_fsync =
        std::strtol(arg_value(argc, argv, "--fullfsync", "0"), nullptr, 10) != 0;
    const std::uint64_t seed = std::strtoull(arg_value(argc, argv, "--seed", "42"), nullptr, 10);

    std::unique_ptr<BenchEngine> engine =
        engine_name == "rocksdb" ? make_rocksdb_engine() : make_strata_engine();
    if (engine == nullptr) {
        std::fprintf(stderr, "engine %s not available in this build\n", engine_name.c_str());
        return 2;
    }
    std::string err;
    if (!engine->open(dir, sync_writes, full_fsync, &err)) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }

    const WorkloadMix mix = mix_for(workload);
    const ycsb::ZipfianGenerator zipf(records);
    // Errors used to be one bool that aborted the run, so a failing engine produced a
    // short run and no count. They are now tallied by kind and the run continues, which
    // is what makes offered work and completed work two different numbers instead of one.
    std::atomic<std::uint64_t> err_put{0};
    std::atomic<std::uint64_t> err_get{0};
    std::atomic<std::uint64_t> err_scan{0};
    // A read that returns "no such key" for a key the load phase wrote is a correctness
    // failure, not a fast read. The old driver passed &found and then ignored it, so an
    // engine that answered every lookup with a miss would have reported a throughput
    // record. Counting it is what holds correctness fixed while the numbers are compared.
    std::atomic<std::uint64_t> read_miss{0};
    std::atomic<std::uint64_t> insert_sequence{records}; // workload e appends

    // Mixed workloads (e = 95% scan / 5% insert) hide which operation owns a
    // tail percentile, so latencies are bucketed by operation kind as well as
    // pooled. Without this split an insert-path stall reads as a scan tail.
    enum OpKind { kRead = 0, kUpdate, kScan, kInsert, kOpKinds };
    static const char* const kOpNames[kOpKinds] = {"read", "update", "scan", "insert"};
    std::vector<std::vector<std::uint64_t>> latencies(threads);
    std::vector<std::array<std::vector<std::uint64_t>, kOpKinds>> by_kind(threads);
    const std::uint64_t wall_start = now_ns();

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            ycsb::Rng rng(seed * 1315423911u + t + 1);
            const std::uint64_t ops = total_ops / threads;
            auto& lat = latencies[t];
            auto& kinds = by_kind[t];
            lat.reserve(ops);
            std::string value_scratch;

            for (std::uint64_t i = 0; i < ops; ++i) {
                int kind = kInsert;
                const std::uint64_t op_start = now_ns();
                if (workload == "load") {
                    // Partitioned sequential insert of the whole key space.
                    const std::uint64_t index = t * ops + i;
                    if (!engine->put(ycsb::key_name(index), ycsb::make_value(rng, value_size))) {
                        err_put.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    const int dice = static_cast<int>(rng.uniform(100));
                    if (dice < mix.read_pct) {
                        kind = kRead;
                        bool found = false;
                        if (!engine->get(ycsb::key_name(zipf.next_scrambled(rng)), &value_scratch,
                                         &found)) {
                            err_get.fetch_add(1, std::memory_order_relaxed);
                        } else if (!found) {
                            // Every key a read can select was written by the load phase,
                            // so a miss here means the store lost a row.
                            read_miss.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (dice < mix.read_pct + mix.update_pct) {
                        kind = kUpdate;
                        if (!engine->put(ycsb::key_name(zipf.next_scrambled(rng)),
                                         ycsb::make_value(rng, value_size))) {
                            err_put.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (dice < mix.read_pct + mix.update_pct + mix.scan_pct) {
                        kind = kScan;
                        const int len = 1 + static_cast<int>(rng.uniform(100));
                        if (engine->scan(ycsb::key_name(zipf.next_scrambled(rng)), len) < 0) {
                            err_scan.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        const std::uint64_t fresh =
                            insert_sequence.fetch_add(1, std::memory_order_relaxed);
                        if (!engine->put(ycsb::key_name(fresh),
                                         ycsb::make_value(rng, value_size))) {
                            err_put.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                const std::uint64_t elapsed = now_ns() - op_start;
                lat.push_back(elapsed);
                kinds[kind].push_back(elapsed);
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    const double secs = static_cast<double>(now_ns() - wall_start) / 1e9;
    const double cpu = cpu_seconds();
    const std::uint64_t rss_steady = current_rss_bytes();
    const std::uint64_t rss_peak = peak_rss_bytes();

    std::vector<std::uint64_t> all;
    for (auto& lat : latencies) {
        all.insert(all.end(), lat.begin(), lat.end());
    }
    std::sort(all.begin(), all.end());
    const auto pct = [&](double p) {
        if (all.empty()) {
            return 0.0;
        }
        const std::size_t idx =
            std::min(all.size() - 1, static_cast<std::size_t>(p * static_cast<double>(all.size())));
        return static_cast<double>(all[idx]) / 1000.0; // us
    };

    std::printf("engine=%s workload=%s records=%llu ops=%llu threads=%u value=%zu sync=%d\n",
                engine_name.c_str(), workload.c_str(), static_cast<unsigned long long>(records),
                static_cast<unsigned long long>(all.size()), threads, value_size,
                sync_writes ? 1 : 0);
    std::printf("  wall=%.2fs throughput=%.0f ops/s p50=%.1fus p95=%.1fus p99=%.1fus "
                "p999=%.1fus\n",
                secs, static_cast<double>(all.size()) / secs, pct(0.50), pct(0.95), pct(0.99),
                pct(0.999));
    for (int k = 0; k < kOpKinds; ++k) {
        std::vector<std::uint64_t> v;
        for (auto& per_thread : by_kind) {
            v.insert(v.end(), per_thread[k].begin(), per_thread[k].end());
        }
        if (v.empty()) {
            continue;
        }
        std::sort(v.begin(), v.end());
        const auto kp = [&](double p) {
            const std::size_t i =
                std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())));
            return static_cast<double>(v[i]) / 1000.0;
        };
        std::printf("  %-6s n=%-8zu p50=%.1fus p95=%.1fus p99=%.1fus p999=%.1fus max=%.1fus\n",
                    kOpNames[k], v.size(), kp(0.50), kp(0.95), kp(0.99), kp(0.999),
                    static_cast<double>(v.back()) / 1000.0);
    }
    std::printf("  %s\n", engine->stats_summary().c_str());

    // The three things a throughput line alone never says: what failed, what it held in
    // memory, and what it cost. USD is a derived figure, not a measurement: the measured
    // quantity is cpu_s, and the rate it is multiplied by is printed with it so the
    // arithmetic can be redone against a different price without rerunning the benchmark.
    const std::uint64_t errors =
        err_put.load() + err_get.load() + err_scan.load() + read_miss.load();
    const double usd_per_cpu_s = 0.145 / 4.0 / 3600.0; // c7g.xlarge on-demand / 4 vCPU
    std::printf("  offered=%llu completed=%llu errors=%llu (put=%llu get=%llu scan=%llu "
                "read_miss=%llu)\n",
                static_cast<unsigned long long>(total_ops),
                static_cast<unsigned long long>(all.size() - errors),
                static_cast<unsigned long long>(errors),
                static_cast<unsigned long long>(err_put.load()),
                static_cast<unsigned long long>(err_get.load()),
                static_cast<unsigned long long>(err_scan.load()),
                static_cast<unsigned long long>(read_miss.load()));
    std::printf("  rss_peak_mb=%.1f rss_steady_mb=%.1f cpu_s=%.2f cpu_us_per_op=%.2f "
                "usd_per_million_ops=%.6f rate=c7g.xlarge_0.145usd_hr_4vcpu\n",
                static_cast<double>(rss_peak) / 1048576.0,
                static_cast<double>(rss_steady) / 1048576.0, cpu,
                all.empty() ? 0.0 : cpu * 1e6 / static_cast<double>(all.size()),
                all.empty() ? 0.0 : cpu * usd_per_cpu_s * 1e6 / static_cast<double>(all.size()));

    engine->close();
    return errors == 0 ? 0 : 3;
}
