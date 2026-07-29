#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/snapshot.h"
#include "db/table_cache.h"
#include "db/version.h"
#include "strata/db.h"
#include "util/cache.h"
#include "util/env.h"
#include "wal/wal_writer.h"

namespace strata {

class DBImpl final : public DB {
  public:
    DBImpl(const Options& options, std::string dbname);
    ~DBImpl() override;

    // Lock, recover (MANIFEST + orphan cleanup + WAL replay), start threads.
    Status init();

    Status put(const WriteOptions& opt, const Slice& key, const Slice& value) override;
    Status remove(const WriteOptions& opt, const Slice& key) override;
    Status write(const WriteOptions& opt, WriteBatch* updates) override;
    Status get(const ReadOptions& opt, const Slice& key, std::string* value) override;
    Iterator* new_iterator(const ReadOptions& opt) override;
    const Snapshot* get_snapshot() override;
    void release_snapshot(const Snapshot* snapshot) override;
    Status flush() override;
    Status compact_all() override;
    DbStats stats() const override;

    // Test hook: one manual compaction round at `level` (the same engine
    // compact_all uses); lets tests build adversarial level shapes.
    Status compact_level_for_test(int level);

  private:
    struct Writer {
        explicit Writer(WriteBatch* b) : batch(b) {}
        WriteBatch* batch;
        Status status;
        bool done = false;
        std::condition_variable cv;
    };

    struct InternalStats {
        std::atomic<std::uint64_t> user_bytes{0};
        std::atomic<std::uint64_t> wal_bytes{0};
        std::atomic<std::uint64_t> flush_bytes{0};
        std::atomic<std::uint64_t> compaction_bytes_read{0};
        std::atomic<std::uint64_t> compaction_bytes_written{0};
        std::atomic<std::uint64_t> flush_count{0};
        std::atomic<std::uint64_t> compaction_count{0};
        std::atomic<std::uint64_t> stall_micros{0};
    };

    // --- write path (docs/DESIGN.md §2.2) ---
    // REQUIRES lock held; may release and reacquire. Stalls per backpressure
    // rules, rotates memtable+WAL when full.
    Status make_room_for_write(std::unique_lock<std::mutex>& lock);
    WriteBatch* build_batch_group(Writer** last_writer); // lock held
    Status rotate_memtable_and_wal();                    // lock held
    Status sync_wal_for_policy();                        // no lock needed

    // --- background work ---
    void flush_thread_main();
    void compaction_thread_main();
    void wal_sync_thread_main(); // FsyncPolicy::kInterval tick
    // Lock held on entry and exit; releases it around file I/O.
    Status flush_oldest_immutable(std::unique_lock<std::mutex>& lock);
    Status do_compaction(CompactionJob* job, std::unique_lock<std::mutex>& lock);
    // Builds one L0 table from mem. Lock must NOT be held.
    Status write_level0_table(const std::shared_ptr<MemTable>& mem,
                              std::shared_ptr<FileMeta>* meta);
    bool is_base_level_for_key(const Version& v, int first_level, const Slice& ukey,
                               std::vector<std::size_t>* level_ptrs) const;
    void remove_obsolete_files(std::unique_lock<std::mutex>& lock); // lock held
    void record_background_error(const Status& s);                  // lock held

    // --- recovery ---
    Status recover_wal_files();

    // Immutable after init().
    Options options_;
    Env* env_ = nullptr;
    const std::string dbname_;
    InternalKeyComparator icmp_;
    std::unique_ptr<BlockCache> block_cache_;
    TableReadStats table_stats_;
    std::unique_ptr<TableCache> table_cache_;
    std::unique_ptr<VersionSet> versions_;
    FileLock* db_lock_ = nullptr;

    std::mutex mutex_;
    std::condition_variable bg_work_cv_; // flush/compaction threads wait here
    std::condition_variable stall_cv_;   // stalled writers + flush() waiters
    std::condition_variable manual_cv_;  // compact_all coordination

    std::deque<Writer*> writers_;
    WriteBatch tmp_batch_; // group-commit scratch (leader-only access)

    std::shared_ptr<MemTable> mem_;
    std::vector<std::shared_ptr<MemTable>> imms_; // oldest first
    std::unique_ptr<WalWriter> wal_;
    std::uint64_t wal_number_ = 0;
    std::uint64_t wal_unsynced_bytes_ = 0;

    SnapshotList snapshots_;
    std::set<std::uint64_t> pending_outputs_; // in-flight SST file numbers
    Status bg_error_;
    bool shutting_down_ = false;
    bool flush_in_progress_ = false;
    bool compaction_in_progress_ = false;
    int manual_compact_level_ = -1;

    std::thread flush_thread_;
    std::thread compaction_thread_;
    std::thread wal_sync_thread_;

    mutable InternalStats stats_;
};

} // namespace strata
