// Compaction concurrency benchmark: fixed update-heavy dataset ingested by
// concurrent writers, then time-to-quiescence for the background compaction
// backlog, then a point-read phase. Reports ingest throughput, write-stall
// time, compaction wall time, write amplification, read p50/p99, peak RSS,
// average CPU cores used, and final database size.
//
// Build STRATA_BENCH_BASELINE to run the same workload against a strata tree
// that predates BackgroundConfig (the serial-compaction baseline): it uses
// the public DB::open and ignores --workers.
//
// Usage: compaction_bench --db DIR [--workers N] [--threads N] [--ops N]
//                         [--keys N] [--value-bytes N] [--reads N]
//                         [--seed N] [--label STR]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "strata/db.h"
#include "strata/options.h"

#ifndef STRATA_BENCH_BASELINE
#include "db/db_impl.h"
#endif

namespace {

struct Config {
    std::string db_dir;
    std::string label = "strata";
    int workers = 1;
    int threads = 4;
    std::uint64_t ops = 3000000;
    std::uint64_t keys = 2000000;
    std::size_t value_bytes = 100;
    std::uint64_t reads = 200000;
    std::uint64_t seed = 42;
};

double now_secs() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double cpu_secs() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const auto tv = [](const timeval& t) {
        return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6;
    };
    return tv(ru.ru_utime) + tv(ru.ru_stime);
}

std::uint64_t peak_rss_bytes() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::uint64_t>(ru.ru_maxrss); // bytes on macOS
}

std::uint64_t dir_bytes(const std::string& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            total += static_cast<std::uint64_t>(e.file_size(ec));
        }
    }
    return total;
}

std::string key_of(std::uint64_t k) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "k%010" PRIu64, k);
    return std::string(buf);
}

int fail(const char* msg) {
    std::fprintf(stderr, "compaction_bench: %s\n", msg);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* {
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        if (arg == "--db") {
            const char* v = next();
            if (v == nullptr) {
                return fail("--db needs a value");
            }
            cfg.db_dir = v;
        } else if (arg == "--workers") {
            cfg.workers = std::atoi(next());
        } else if (arg == "--threads") {
            cfg.threads = std::atoi(next());
        } else if (arg == "--ops") {
            cfg.ops = std::strtoull(next(), nullptr, 10);
        } else if (arg == "--keys") {
            cfg.keys = std::strtoull(next(), nullptr, 10);
        } else if (arg == "--value-bytes") {
            cfg.value_bytes = std::strtoull(next(), nullptr, 10);
        } else if (arg == "--reads") {
            cfg.reads = std::strtoull(next(), nullptr, 10);
        } else if (arg == "--seed") {
            cfg.seed = std::strtoull(next(), nullptr, 10);
        } else if (arg == "--label") {
            cfg.label = next();
        } else {
            return fail(("unknown argument: " + arg).c_str());
        }
    }
    if (cfg.db_dir.empty()) {
        return fail("--db is required");
    }

    strata::Options options;
    options.fsync_policy = strata::FsyncPolicy::kNever; // compaction-bound, not fsync-bound
    options.write_buffer_size = 4u << 20;               // sustained flush/compaction pressure
    options.block_cache_bytes = 64u << 20;

    std::unique_ptr<strata::DB> db;
#ifdef STRATA_BENCH_BASELINE
    {
        strata::DB* raw = nullptr;
        const strata::Status s = strata::DB::open(options, cfg.db_dir, &raw);
        if (!s.ok()) {
            return fail(s.to_string().c_str());
        }
        db.reset(raw);
    }
#else
    {
        strata::BackgroundConfig bg;
        bg.compaction_workers = cfg.workers;
        bg.compaction_queue_capacity = 8;
        auto impl = std::make_unique<strata::DBImpl>(options, cfg.db_dir, bg);
        const strata::Status s = impl->init();
        if (!s.ok()) {
            return fail(s.to_string().c_str());
        }
        db = std::move(impl);
    }
#endif

    const double t0 = now_secs();
    const double cpu0 = cpu_secs();

    // --- Phase 1: concurrent ingest (uniform updates over a fixed keyspace)
    std::atomic<bool> failed{false};
    std::vector<std::thread> writers;
    const std::uint64_t per_thread = cfg.ops / static_cast<std::uint64_t>(cfg.threads);
    for (int t = 0; t < cfg.threads; ++t) {
        writers.emplace_back([&, t] {
            std::mt19937_64 rng(cfg.seed + static_cast<std::uint64_t>(t));
            std::string value(cfg.value_bytes, 'x');
            for (std::uint64_t i = 0; i < per_thread && !failed.load(); ++i) {
                const std::uint64_t k = rng() % cfg.keys;
                // Cheap deterministic-ish value mutation so blocks do not
                // degenerate into one repeated byte.
                value[i % cfg.value_bytes] = static_cast<char>('a' + (k % 26));
                if (!db->put(strata::WriteOptions(), key_of(k), value).ok()) {
                    failed.store(true);
                }
            }
        });
    }
    for (auto& w : writers) {
        w.join();
    }
    if (failed.load()) {
        return fail("write failed during ingest");
    }
    const double t_ingest_end = now_secs();

    // --- Phase 2: wait for the compaction backlog to drain. Quiescence is
    // observed from outside (works identically for the serial baseline):
    // background counters unchanged for 2 s. Compaction wall time is the
    // moment of the LAST counter change minus ingest end, so the 2 s idle
    // confirmation window is not billed to compaction.
    const auto counters = [&] {
        const strata::DbStats s = db->stats();
        return s.flush_count + s.compaction_count + s.compaction_bytes_written +
               s.flush_bytes_written;
    };
    std::uint64_t last = counters();
    double last_change = now_secs();
    while (now_secs() - last_change < 2.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const std::uint64_t cur = counters();
        if (cur != last) {
            last = cur;
            last_change = now_secs();
        }
    }
    const double compact_wall = std::max(0.0, last_change - t_ingest_end);
    const double t_quiesce = now_secs();
    const double cpu_active = cpu_secs() - cpu0;

    // --- Phase 3: point reads (single thread, uniform keys)
    std::vector<double> lat;
    lat.reserve(cfg.reads);
    std::mt19937_64 rng(cfg.seed + 1000);
    std::string out;
    std::uint64_t found = 0;
    for (std::uint64_t i = 0; i < cfg.reads; ++i) {
        const std::uint64_t k = rng() % cfg.keys;
        const double a = now_secs();
        const strata::Status s = db->get(strata::ReadOptions(), key_of(k), &out);
        lat.push_back((now_secs() - a) * 1e6);
        if (s.ok()) {
            ++found;
        } else if (!s.is_not_found()) {
            return fail(s.to_string().c_str());
        }
    }
    std::sort(lat.begin(), lat.end());
    const auto pct = [&](double p) {
        if (lat.empty()) {
            return 0.0;
        }
        const std::size_t idx =
            std::min(lat.size() - 1, static_cast<std::size_t>(p * static_cast<double>(lat.size())));
        return lat[idx];
    };
    const double t_reads_end = now_secs();

    const strata::DbStats st = db->stats();
    const double ingest_secs = t_ingest_end - t0;
    const double read_secs = t_reads_end - t_quiesce;
    const std::uint64_t db_size = dir_bytes(cfg.db_dir);

    std::printf("RESULT label=%s workers=%d threads=%d ops=%" PRIu64 " keys=%" PRIu64
                " value_bytes=%zu\n",
                cfg.label.c_str(), cfg.workers, cfg.threads, cfg.ops, cfg.keys, cfg.value_bytes);
    std::printf("  ingest:     %.2f s, %.0f ops/s\n", ingest_secs,
                static_cast<double>(cfg.ops) / ingest_secs);
    std::printf("  stall:      %.1f ms total write-stall\n",
                static_cast<double>(st.write_stall_micros) / 1000.0);
    std::printf("  compaction: %.2f s wall to quiesce after ingest, %" PRIu64
                " compactions, %" PRIu64 " flushes\n",
                compact_wall, st.compaction_count, st.flush_count);
    std::printf("  write-amp:  %.2f (wal=%" PRIu64 "MB flush=%" PRIu64 "MB compact=%" PRIu64
                "MB / user=%" PRIu64 "MB)\n",
                st.write_amplification(), st.wal_bytes_written >> 20,
                st.flush_bytes_written >> 20, st.compaction_bytes_written >> 20,
                st.user_bytes_written >> 20);
    std::printf("  reads:      %" PRIu64 " gets in %.2f s (%.0f ops/s), %" PRIu64
                " found, p50=%.1f us p99=%.1f us\n",
                cfg.reads, read_secs, static_cast<double>(cfg.reads) / read_secs, found, pct(0.50),
                pct(0.99));
    std::printf("  resources:  peak_rss=%.1f MB, cpu_avg=%.2f cores over ingest+compaction\n",
                static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0),
                cpu_active / (t_quiesce - t0));
    std::printf("  final size: %.1f MB on disk\n", static_cast<double>(db_size) / (1024.0 * 1024.0));
    std::printf("CSV %s,%d,%.2f,%.0f,%.1f,%.2f,%.2f,%.1f,%.1f,%.1f,%.2f,%.1f,%" PRIu64 ",%" PRIu64
                "\n",
                cfg.label.c_str(), cfg.workers, ingest_secs,
                static_cast<double>(cfg.ops) / ingest_secs,
                static_cast<double>(st.write_stall_micros) / 1000.0, compact_wall,
                st.write_amplification(), pct(0.50), pct(0.99),
                static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0),
                cpu_active / (t_quiesce - t0), static_cast<double>(db_size) / (1024.0 * 1024.0),
                st.compaction_count, st.flush_count);
    return 0;
}
