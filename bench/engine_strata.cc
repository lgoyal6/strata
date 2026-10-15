#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>

#include "bench/engine.h"

#include "db/db_impl.h"
#include "strata/db.h"
#include "util/scan_probe.h"

namespace {

class StrataEngine final : public BenchEngine {
  public:
    bool open(const std::string& dir, bool sync_writes, bool full_fsync, bool pin_snapshot,
              std::string* err) override {
        strata::Options options;
        // Bench-only override: a smaller memtable means more L0 files, which is
        // the knob that varies merge fan-in for the linear-vs-heap comparison.
        options.write_buffer_size = 8u << 20;
        if (const char* wb = std::getenv("STRATA_BENCH_WRITE_BUFFER_MB")) {
            const unsigned long mb = std::strtoul(wb, nullptr, 10);
            if (mb > 0) {
                options.write_buffer_size = static_cast<std::size_t>(mb) << 20;
            }
        }
        options.block_cache_bytes = 64u << 20;
        options.bloom_bits_per_key = 10;
        options.fsync_policy =
            sync_writes ? strata::FsyncPolicy::kAlways : strata::FsyncPolicy::kNever;
        options.use_fullfsync = full_fsync;
        int workers = 1;
        if (const char* configured = std::getenv("STRATA_BENCH_COMPACTION_WORKERS")) {
            workers = std::atoi(configured);
            if (workers < 1)
                workers = 1;
        }
        auto impl =
            std::make_unique<strata::DBImpl>(options, dir, strata::BackgroundConfig{workers, 4});
        const strata::Status s = impl->init();
        if (!s.ok()) {
            *err = s.to_string();
            return false;
        }
        db_ = std::move(impl);
        if (pin_snapshot)
            snapshot_ = db_->get_snapshot();
        return true;
    }

    bool put(const std::string& key, const std::string& value) override {
        return db_->put(strata::WriteOptions(), key, value).ok();
    }

    bool get(const std::string& key, std::string* value, bool* found) override {
        strata::ReadOptions options;
        options.snapshot = snapshot_;
        const strata::Status s = db_->get(options, key, value);
        if (s.ok()) {
            *found = true;
            return true;
        }
        if (s.is_not_found()) {
            *found = false;
            return true;
        }
        return false;
    }

    int scan(const std::string& start_key, int n) override {
        const std::unique_ptr<strata::Iterator> it(db_->new_iterator(strata::ReadOptions()));
        int touched = 0;
        for (it->seek(start_key); it->valid() && touched < n; it->next()) {
            (void)it->key();
            (void)it->value();
            ++touched;
        }
        if (!it->status().ok()) {
            return -1;
        }
        return touched;
    }

    std::uint64_t live_sst_bytes() override {
        return db_->stats().current_sst_bytes;
    }

    std::string stats_summary() override {
        const strata::DbStats s = db_->stats();
        std::ostringstream out;
        out << "strata: user_mb=" << (s.user_bytes_written >> 20)
            << " wal_mb=" << (s.wal_bytes_written >> 20)
            << " flush_mb=" << (s.flush_bytes_written >> 20)
            << " compact_write_mb=" << (s.compaction_bytes_written >> 20)
            << " write_amp=" << s.write_amplification() << " flushes=" << s.flush_count
            << " compactions=" << s.compaction_count
            << " stall_ms=" << (s.write_stall_micros / 1000) << " bloom_skip_rate="
            << (s.bloom_checks
                    ? static_cast<double>(s.bloom_skips) / static_cast<double>(s.bloom_checks)
                    : 0.0)
            << " block_cache_hit_rate="
            << (s.block_cache_hits + s.block_cache_misses
                    ? static_cast<double>(s.block_cache_hits) /
                          static_cast<double>(s.block_cache_hits + s.block_cache_misses)
                    : 0.0);
        out << " block_cache_hits=" << s.block_cache_hits
            << " block_cache_misses=" << s.block_cache_misses;
        out << " point_reads=" << s.point_reads << " sst_file_probes=" << s.sst_file_probes
            << " compaction_debt_bytes=" << s.compaction_debt_bytes
            << " current_sst_bytes=" << s.current_sst_bytes << " snapshots=" << s.snapshot_count;
        if (s.point_reads > 0) {
            out << " sst_probes_per_point_read="
                << static_cast<double>(s.sst_file_probes) / static_cast<double>(s.point_reads);
        }
#ifdef STRATA_SCAN_PROBE
        {
            auto& c = strata::probe::counters();
            const double scans = static_cast<double>(c.scans.load());
            if (scans > 0) {
                out << "\n  scan-probe: scans=" << c.scans.load()
                    << " children/scan=" << (static_cast<double>(c.children.load()) / scans)
                    << " l0children/scan=" << (static_cast<double>(c.l0_children.load()) / scans)
                    << " blockreads/scan=" << (static_cast<double>(c.block_reads.load()) / scans)
                    << " blockmisses/scan=" << (static_cast<double>(c.block_misses.load()) / scans)
                    << " internal_next/scan="
                    << (static_cast<double>(c.internal_next.load()) / scans)
                    << " skipped/scan=" << (static_cast<double>(c.skipped.load()) / scans)
                    << " skipped_seq/scan=" << (static_cast<double>(c.skipped_seq.load()) / scans)
                    << " skipped_key/scan=" << (static_cast<double>(c.skipped_key.load()) / scans)
                    << " max_same_key_run=" << c.max_run.load()
                    << " skip_seeks/scan=" << (static_cast<double>(c.skip_seeks.load()) / scans)
                    << " compares/scan=" << (static_cast<double>(c.compares.load()) / scans);
            }
        }
#endif
        return out.str();
    }

    void close() override {
        if (snapshot_ != nullptr) {
            db_->release_snapshot(snapshot_);
            snapshot_ = nullptr;
        }
        db_.reset();
    }

  private:
    std::unique_ptr<strata::DB> db_;
    const strata::Snapshot* snapshot_ = nullptr;
};

} // namespace

std::unique_ptr<BenchEngine> make_strata_engine() {
    return std::make_unique<StrataEngine>();
}
