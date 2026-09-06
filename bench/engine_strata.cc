#include <cstdlib>
#include <memory>
#include <sstream>

#include "bench/engine.h"

#include "strata/db.h"
#include "util/scan_probe.h"

namespace {

class StrataEngine final : public BenchEngine {
  public:
    bool open(const std::string& dir, bool sync_writes, bool full_fsync,
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
        strata::DB* db = nullptr;
        const strata::Status s = strata::DB::open(options, dir, &db);
        if (!s.ok()) {
            *err = s.to_string();
            return false;
        }
        db_.reset(db);
        return true;
    }

    bool put(const std::string& key, const std::string& value) override {
        return db_->put(strata::WriteOptions(), key, value).ok();
    }

    bool get(const std::string& key, std::string* value, bool* found) override {
        const strata::Status s = db_->get(strata::ReadOptions(), key, value);
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
        db_.reset();
    }

  private:
    std::unique_ptr<strata::DB> db_;
};

} // namespace

std::unique_ptr<BenchEngine> make_strata_engine() {
    return std::make_unique<StrataEngine>();
}
