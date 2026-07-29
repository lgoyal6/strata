#include <memory>
#include <sstream>

#include "bench/engine.h"

#ifdef STRATA_HAVE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>

namespace {

class RocksEngine final : public BenchEngine {
  public:
    bool open(const std::string& dir, bool sync_writes, bool /*full_fsync*/,
              std::string* err) override {
        // RocksDB on macOS always uses F_FULLFSYNC for sync writes.
        rocksdb::Options options;
        options.create_if_missing = true;
        // Fairness: match strata's knobs, disable compression (strata has
        // none), keep everything else at RocksDB defaults.
        options.compression = rocksdb::kNoCompression;
        options.write_buffer_size = 8u << 20;
        options.target_file_size_base = 8u << 20;
        options.max_bytes_for_level_base = 64u << 20;
        options.level0_file_num_compaction_trigger = 4;
        options.level0_slowdown_writes_trigger = 8;
        options.level0_stop_writes_trigger = 12;
        options.max_background_jobs = 2; // strata: flush thread + compaction thread

        rocksdb::BlockBasedTableOptions table;
        table.block_size = 4096;
        table.block_cache = rocksdb::NewLRUCache(64u << 20);
        table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));

        sync_writes_ = sync_writes;
        const rocksdb::Status s = rocksdb::DB::Open(options, dir, &db_);
        if (!s.ok()) {
            *err = s.ToString();
            return false;
        }
        return true;
    }

    bool put(const std::string& key, const std::string& value) override {
        rocksdb::WriteOptions wo;
        wo.sync = sync_writes_;
        return db_->Put(wo, key, value).ok();
    }

    bool get(const std::string& key, std::string* value, bool* found) override {
        const rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, value);
        if (s.ok()) {
            *found = true;
            return true;
        }
        if (s.IsNotFound()) {
            *found = false;
            return true;
        }
        return false;
    }

    int scan(const std::string& start_key, int n) override {
        const std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        int touched = 0;
        for (it->Seek(start_key); it->Valid() && touched < n; it->Next()) {
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
        // rocksdb.stats is maintained without a Statistics object, so the
        // timing runs stay overhead-free; write-amp numbers come from its
        // cumulative compaction/WAL lines.
        std::string stats;
        db_->GetProperty("rocksdb.stats", &stats);
        std::ostringstream out;
        out << "rocksdb raw stats below\n" << stats;
        return out.str();
    }

    void close() override {
        db_.reset();
    }

  private:
    std::unique_ptr<rocksdb::DB> db_;
    bool sync_writes_ = false;
};

} // namespace

std::unique_ptr<BenchEngine> make_rocksdb_engine() {
    return std::make_unique<RocksEngine>();
}

#else

std::unique_ptr<BenchEngine> make_rocksdb_engine() {
    return nullptr;
}

#endif
