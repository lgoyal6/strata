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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> insert_sequence{records}; // workload e appends

    std::vector<std::vector<std::uint64_t>> latencies(threads);
    const std::uint64_t wall_start = now_ns();

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            ycsb::Rng rng(seed * 1315423911u + t + 1);
            const std::uint64_t ops = total_ops / threads;
            auto& lat = latencies[t];
            lat.reserve(ops);
            std::string value_scratch;

            for (std::uint64_t i = 0; i < ops && !failed.load(std::memory_order_relaxed); ++i) {
                bool ok = true;
                const std::uint64_t op_start = now_ns();
                if (workload == "load") {
                    // Partitioned sequential insert of the whole key space.
                    const std::uint64_t index = t * ops + i;
                    ok = engine->put(ycsb::key_name(index), ycsb::make_value(rng, value_size));
                } else {
                    const int dice = static_cast<int>(rng.uniform(100));
                    if (dice < mix.read_pct) {
                        bool found = false;
                        ok = engine->get(ycsb::key_name(zipf.next_scrambled(rng)), &value_scratch,
                                         &found);
                    } else if (dice < mix.read_pct + mix.update_pct) {
                        ok = engine->put(ycsb::key_name(zipf.next_scrambled(rng)),
                                         ycsb::make_value(rng, value_size));
                    } else if (dice < mix.read_pct + mix.update_pct + mix.scan_pct) {
                        const int len = 1 + static_cast<int>(rng.uniform(100));
                        ok = engine->scan(ycsb::key_name(zipf.next_scrambled(rng)), len) >= 0;
                    } else {
                        const std::uint64_t fresh =
                            insert_sequence.fetch_add(1, std::memory_order_relaxed);
                        ok = engine->put(ycsb::key_name(fresh), ycsb::make_value(rng, value_size));
                    }
                }
                lat.push_back(now_ns() - op_start);
                if (!ok) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    const double secs = static_cast<double>(now_ns() - wall_start) / 1e9;

    if (failed.load()) {
        std::fprintf(stderr, "workload hit an engine error\n");
        return 1;
    }

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
    std::printf("  %s\n", engine->stats_summary().c_str());
    engine->close();
    return 0;
}
