#include "db/db_impl.h"

#include <algorithm>
#include <cassert>

#include "db/db_iter.h"
#include "db/write_batch_internal.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"
#include "wal/wal_reader.h"

namespace strata {

// ===========================================================================
// Open / close
// ===========================================================================

Status DB::open(const Options& options, const std::string& dbname, DB** dbptr) {
    *dbptr = nullptr;
    auto impl = std::make_unique<DBImpl>(options, dbname);
    const Status s = impl->init();
    if (!s.ok()) {
        return s;
    }
    *dbptr = impl.release();
    return Status::okay();
}

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options), dbname_(std::move(dbname)) {
    env_ = options_.env != nullptr ? options_.env : Env::default_env();
    options_.env = env_;
    // Sanitize: a write buffer at or below one arena block (4 KiB) would
    // rotate the memtable on every write.
    options_.write_buffer_size = std::max<std::size_t>(options_.write_buffer_size, 16 * 1024);
    options_.max_immutable_memtables = std::max(options_.max_immutable_memtables, 1);
    options_.l0_slowdown_trigger =
        std::max(options_.l0_slowdown_trigger, options_.l0_compaction_trigger);
    options_.l0_stop_trigger = std::max(options_.l0_stop_trigger, options_.l0_slowdown_trigger);
    if (options_.block_cache_bytes > 0) {
        block_cache_ = std::make_unique<BlockCache>(options_.block_cache_bytes);
    }
    table_cache_ = std::make_unique<TableCache>(env_, dbname_, options_, icmp_, block_cache_.get(),
                                                &table_stats_);
    versions_ = std::make_unique<VersionSet>(env_, dbname_, &options_, table_cache_.get(), &icmp_);
}

DBImpl::~DBImpl() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        shutting_down_ = true;
        bg_work_cv_.notify_all();
        stall_cv_.notify_all();
        manual_cv_.notify_all();
    }
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    if (compaction_thread_.joinable()) {
        compaction_thread_.join();
    }
    if (wal_sync_thread_.joinable()) {
        wal_sync_thread_.join();
    }
    wal_.reset(); // flushes buffered bytes to the kernel and closes
    if (db_lock_ != nullptr) {
        env_->unlock_file(db_lock_);
        db_lock_ = nullptr;
    }
}

Status DBImpl::init() {
    Status s = env_->create_dir_if_missing(dbname_);
    if (!s.ok()) {
        return s;
    }
    s = env_->lock_file(lock_file_name(dbname_), &db_lock_);
    if (!s.ok()) {
        return s;
    }
    bool created_new = false;
    s = versions_->recover(&created_new);
    if (!s.ok()) {
        return s;
    }
    s = recover_wal_files();
    if (!s.ok()) {
        return s;
    }
    {
        // Orphan SSTs / stale tmp / obsolete WALs from before the crash.
        std::unique_lock<std::mutex> lock(mutex_);
        remove_obsolete_files(lock);
    }

    flush_thread_ = std::thread(&DBImpl::flush_thread_main, this);
    compaction_thread_ = std::thread(&DBImpl::compaction_thread_main, this);
    if (options_.fsync_policy == FsyncPolicy::kInterval) {
        wal_sync_thread_ = std::thread(&DBImpl::wal_sync_thread_main, this);
    }
    return Status::okay();
}

// ===========================================================================
// Recovery (docs/DESIGN.md §1.3): replay every record of every WAL >=
// min_wal_number — no sequence filtering.
// ===========================================================================

Status DBImpl::recover_wal_files() {
    std::vector<std::string> children;
    Status s = env_->get_children(dbname_, &children);
    if (!s.ok()) {
        return s;
    }
    std::uint64_t max_number = 0;
    std::vector<std::uint64_t> wals;
    for (const auto& name : children) {
        std::uint64_t number = 0;
        FileType type = FileType::kUnknown;
        if (parse_file_name(name, &number, &type)) {
            max_number = std::max(max_number, number);
            if (type == FileType::kWal && number >= versions_->min_wal_number()) {
                wals.push_back(number);
            }
        }
    }
    // Orphans minted after the last MANIFEST write may exceed its counter.
    versions_->bump_file_number_floor(max_number + 1);
    std::sort(wals.begin(), wals.end());

    SequenceNumber max_seq = versions_->last_sequence();
    std::shared_ptr<MemTable> mem;

    for (std::size_t i = 0; i < wals.size(); ++i) {
        const std::uint64_t number = wals[i];
        std::unique_ptr<WalReader> reader;
        s = WalReader::open(env_, wal_file_name(dbname_, number), versions_->db_uuid(), &reader);
        if (!s.ok()) {
            return s; // foreign or corrupt WAL header: refuse to open
        }
        std::string record;
        while (reader->read_record(&record)) {
            s = WriteBatchInternal::check(Slice(record));
            if (!s.ok()) {
                // CRC-valid but structurally bad: real corruption, not a torn
                // tail — refuse to guess.
                return s;
            }
            WriteBatch batch;
            WriteBatchInternal::set_contents(&batch, Slice(record));
            if (mem == nullptr) {
                mem = std::make_shared<MemTable>(icmp_);
                mem->wal_number = number;
            }
            s = WriteBatchInternal::insert_into(&batch, mem.get());
            if (!s.ok()) {
                return s;
            }
            max_seq = std::max(max_seq, WriteBatchInternal::sequence(&batch) +
                                            WriteBatchInternal::count(&batch) - 1);

            if (mem->approximate_memory_usage() > options_.write_buffer_size) {
                std::shared_ptr<FileMeta> meta;
                s = write_level0_table(mem, &meta);
                if (s.ok()) {
                    VersionEdit edit;
                    if (meta != nullptr) {
                        // min_wal must NOT advance: records later in this WAL
                        // are not yet replayed.
                        edit.added_files.emplace_back(0, meta);
                    }
                    s = versions_->log_and_apply(&edit);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (meta != nullptr) {
                        pending_outputs_.erase(meta->number);
                    }
                }
                if (!s.ok()) {
                    return s;
                }
                stats_.flush_bytes.fetch_add(meta != nullptr ? meta->file_size : 0);
                stats_.flush_count.fetch_add(1);
                mem.reset();
            }
        }
        // A torn tail can only be the last record ever written, i.e. in the
        // final WAL: rotation happens strictly after a completed record, and
        // any earlier recovery durably truncated the tear it stopped at
        // (below). A tear anywhere else is real corruption.
        if (reader->truncated_tail()) {
            if (i + 1 != wals.size()) {
                return Status::corruption(wal_file_name(dbname_, number) +
                                          ": torn record in a non-final WAL");
            }
            // Cut the torn bytes so this tear cannot resurface as a
            // mid-sequence error after the next crash.
            const std::uint64_t keep = reader->valid_offset();
            reader.reset(); // close before truncating
            s = env_->truncate_file(wal_file_name(dbname_, number), keep);
            if (!s.ok()) {
                return s;
            }
        }
    }

    versions_->set_last_sequence(max_seq);

    wal_number_ = versions_->new_file_number();
    s = WalWriter::create(env_, wal_file_name(dbname_, wal_number_), versions_->db_uuid(), &wal_);
    if (!s.ok()) {
        return s;
    }
    if (mem != nullptr) {
        // Leftover replay data: keeps its oldest contributing WAL number so
        // min_wal_number only advances once this memtable is flushed.
        mem_ = std::move(mem);
    } else {
        mem_ = std::make_shared<MemTable>(icmp_);
        mem_->wal_number = wal_number_;
        if (!wals.empty()) {
            // Everything replayed was flushed durably: retire the old WALs.
            VersionEdit edit;
            edit.min_wal_number = wal_number_;
            s = versions_->log_and_apply(&edit);
            if (!s.ok()) {
                return s;
            }
        }
    }
    return Status::okay();
}

// ===========================================================================
// Write path (docs/DESIGN.md §2.2)
// ===========================================================================

Status DBImpl::put(const WriteOptions& opt, const Slice& key, const Slice& value) {
    WriteBatch batch;
    batch.put(key, value);
    return write(opt, &batch);
}

Status DBImpl::remove(const WriteOptions& opt, const Slice& key) {
    WriteBatch batch;
    batch.remove(key);
    return write(opt, &batch);
}

Status DBImpl::write(const WriteOptions& /*opt*/, WriteBatch* updates) {
    Writer w(updates);
    std::unique_lock<std::mutex> lock(mutex_);
    writers_.push_back(&w);
    while (!w.done && &w != writers_.front()) {
        w.cv.wait(lock);
    }
    if (w.done) {
        return w.status;
    }

    // This writer is the leader: commit for the whole queue prefix.
    Status s = make_room_for_write(lock);
    Writer* last_writer = &w;
    if (s.ok()) {
        WriteBatch* group = build_batch_group(&last_writer);
        const SequenceNumber first_seq = versions_->last_sequence() + 1;
        WriteBatchInternal::set_sequence(group, first_seq);
        const SequenceNumber last_seq = first_seq + WriteBatchInternal::count(group) - 1;
        const Slice contents = WriteBatchInternal::contents(group);
        const std::uint64_t record_bytes = contents.size() + 9;
        const std::size_t group_user_bytes = group->user_bytes();
        MemTable* mem = mem_.get();

        lock.unlock();
        // WAL first (append + flush to kernel), then per-policy sync, then
        // the memtable. The leader is the only writer here.
        s = wal_->add_record(contents);
        if (s.ok()) {
            s = sync_wal_for_policy();
        }
        if (s.ok()) {
            s = WriteBatchInternal::insert_into(group, mem);
        }
        lock.lock();

        if (s.ok()) {
            // Publish AFTER the memtable apply: readers can never observe a
            // half-applied batch.
            versions_->set_last_sequence(last_seq);
            stats_.user_bytes.fetch_add(group_user_bytes, std::memory_order_relaxed);
            stats_.wal_bytes.fetch_add(record_bytes, std::memory_order_relaxed);
            if (options_.fsync_policy == FsyncPolicy::kInterval) {
                wal_unsynced_bytes_ += record_bytes;
            }
        } else {
            // The WAL may hold a half-written record; refusing further
            // writes keeps the on-disk prefix consistent.
            record_background_error(s);
        }
        if (group == &tmp_batch_) {
            tmp_batch_.clear();
        }
    }

    while (true) {
        Writer* ready = writers_.front();
        writers_.pop_front();
        if (ready != &w) {
            ready->status = s;
            ready->done = true;
            ready->cv.notify_one();
        }
        if (ready == last_writer) {
            break;
        }
    }
    if (!writers_.empty()) {
        writers_.front()->cv.notify_one();
    }
    return s;
}

Status DBImpl::sync_wal_for_policy() {
    if (options_.fsync_policy == FsyncPolicy::kAlways) {
        return wal_->sync(options_.use_fullfsync);
    }
    return Status::okay(); // kInterval: background tick; kNever: nothing
}

WriteBatch* DBImpl::build_batch_group(Writer** last_writer) {
    Writer* first = writers_.front();
    WriteBatch* result = first->batch;
    std::size_t size = WriteBatchInternal::byte_size(first->batch);

    // Cap group size; keep latency low when the lead batch is small.
    std::size_t max_size = 1u << 20;
    if (size <= (128u << 10)) {
        max_size = size + (128u << 10);
    }

    *last_writer = first;
    auto it = writers_.begin();
    ++it;
    for (; it != writers_.end(); ++it) {
        Writer* w = *it;
        if (size + WriteBatchInternal::byte_size(w->batch) > max_size) {
            break;
        }
        if (result == first->batch) {
            result = &tmp_batch_;
            assert(WriteBatchInternal::count(result) == 0);
            WriteBatchInternal::append(result, first->batch);
        }
        WriteBatchInternal::append(result, w->batch);
        size += WriteBatchInternal::byte_size(w->batch);
        *last_writer = w;
    }
    return result;
}

Status DBImpl::make_room_for_write(std::unique_lock<std::mutex>& lock) {
    bool allow_delay = true;
    while (true) {
        if (!bg_error_.ok()) {
            return bg_error_;
        }
        const int l0_files = static_cast<int>(versions_->current()->files[0].size());
        if (allow_delay && l0_files >= options_.l0_slowdown_trigger &&
            l0_files < options_.l0_stop_trigger) {
            // Soft backpressure: hand the CPU to the compaction thread once.
            lock.unlock();
            env_->sleep_micros(1000);
            stats_.stall_micros.fetch_add(1000, std::memory_order_relaxed);
            allow_delay = false;
            lock.lock();
            continue;
        }
        if (mem_->approximate_memory_usage() <= options_.write_buffer_size) {
            return Status::okay();
        }
        if (imms_.size() >= static_cast<std::size_t>(options_.max_immutable_memtables)) {
            // Hard backpressure: stall rather than queue unbounded memory.
            const std::uint64_t t0 = env_->now_micros();
            stall_cv_.wait(lock);
            stats_.stall_micros.fetch_add(env_->now_micros() - t0, std::memory_order_relaxed);
            continue;
        }
        if (l0_files >= options_.l0_stop_trigger) {
            const std::uint64_t t0 = env_->now_micros();
            stall_cv_.wait(lock);
            stats_.stall_micros.fetch_add(env_->now_micros() - t0, std::memory_order_relaxed);
            continue;
        }
        const Status s = rotate_memtable_and_wal();
        if (!s.ok()) {
            return s;
        }
    }
}

Status DBImpl::rotate_memtable_and_wal() {
    const std::uint64_t new_number = versions_->new_file_number();
    std::unique_ptr<WalWriter> new_wal;
    const Status s =
        WalWriter::create(env_, wal_file_name(dbname_, new_number), versions_->db_uuid(), &new_wal);
    if (!s.ok()) {
        return s;
    }
    wal_ = std::move(new_wal);
    wal_number_ = new_number;
    wal_unsynced_bytes_ = 0;

    imms_.push_back(mem_);
    auto fresh = std::make_shared<MemTable>(icmp_);
    fresh->wal_number = new_number;
    mem_ = std::move(fresh);
    bg_work_cv_.notify_all();
    return Status::okay();
}

// ===========================================================================
// Read path
// ===========================================================================

Status DBImpl::get(const ReadOptions& opt, const Slice& key, std::string* value) {
    SequenceNumber seq;
    std::shared_ptr<MemTable> mem;
    std::vector<std::shared_ptr<MemTable>> imms;
    std::shared_ptr<Version> version;
    {
        // One lock hold captures a consistent source stack (mem -> imms ->
        // version); a flush completing in between could otherwise hide data.
        std::lock_guard<std::mutex> lock(mutex_);
        seq = opt.snapshot != nullptr ? static_cast<const SnapshotImpl*>(opt.snapshot)->sequence
                                      : versions_->last_sequence();
        mem = mem_;
        imms = imms_;
        version = versions_->current();
    }

    const LookupKey lkey(key, seq);
    Status s;
    if (mem->get(lkey, value, &s)) {
        return s;
    }
    for (auto it = imms.rbegin(); it != imms.rend(); ++it) {
        if ((*it)->get(lkey, value, &s)) {
            return s;
        }
    }
    return version->get(lkey, value);
}

Iterator* DBImpl::new_iterator(const ReadOptions& opt) {
    SequenceNumber seq;
    std::shared_ptr<MemTable> mem;
    std::vector<std::shared_ptr<MemTable>> imms;
    std::shared_ptr<Version> version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = opt.snapshot != nullptr ? static_cast<const SnapshotImpl*>(opt.snapshot)->sequence
                                      : versions_->last_sequence();
        mem = mem_;
        imms = imms_;
        version = versions_->current();
    }

    std::vector<std::unique_ptr<Iterator>> children;
    children.emplace_back(mem->new_iterator());
    for (auto it = imms.rbegin(); it != imms.rend(); ++it) {
        children.emplace_back((*it)->new_iterator());
    }
    version->add_iterators(&children);

    struct Pin {
        std::shared_ptr<MemTable> mem;
        std::vector<std::shared_ptr<MemTable>> imms;
        std::shared_ptr<Version> version;
    };
    auto pin = std::make_shared<Pin>(Pin{std::move(mem), std::move(imms), version});

    std::unique_ptr<Iterator> internal(new_merging_iterator(&icmp_, std::move(children)));
    return new_db_iterator(std::move(internal), seq, std::move(pin));
}

const Snapshot* DBImpl::get_snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.new_snapshot(versions_->last_sequence());
}

void DBImpl::release_snapshot(const Snapshot* snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.release(static_cast<const SnapshotImpl*>(snapshot));
}

// ===========================================================================
// Flush
// ===========================================================================

Status DBImpl::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!bg_error_.ok()) {
        return bg_error_;
    }
    if (!mem_->empty()) {
        const Status s = rotate_memtable_and_wal();
        if (!s.ok()) {
            return s;
        }
    }
    while ((!imms_.empty() || flush_in_progress_) && bg_error_.ok() && !shutting_down_) {
        stall_cv_.wait(lock);
    }
    return bg_error_;
}

void DBImpl::flush_thread_main() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutting_down_) {
        if (imms_.empty() || !bg_error_.ok()) {
            bg_work_cv_.wait(lock);
            continue;
        }
        flush_in_progress_ = true;
        const Status s = flush_oldest_immutable(lock);
        flush_in_progress_ = false;
        if (!s.ok() && !shutting_down_) {
            record_background_error(s);
        }
        stall_cv_.notify_all();   // stalled writers + flush() waiters
        bg_work_cv_.notify_all(); // L0 grew: compaction thread re-scores
        remove_obsolete_files(lock);
    }
}

Status DBImpl::flush_oldest_immutable(std::unique_lock<std::mutex>& lock) {
    auto mem = imms_.front();

    lock.unlock();
    std::shared_ptr<FileMeta> meta;
    Status s = write_level0_table(mem, &meta);
    lock.lock();

    if (s.ok() && shutting_down_) {
        s = Status::busy("shutting down");
    }
    if (s.ok()) {
        VersionEdit edit;
        if (meta != nullptr) {
            edit.added_files.emplace_back(0, meta);
        }
        // Retiring this memtable makes WALs older than the next unflushed
        // memtable's WAL obsolete. (Durable-before-unlink: log_and_apply
        // syncs the MANIFEST; remove_obsolete_files runs after.)
        edit.min_wal_number = imms_.size() > 1 ? imms_[1]->wal_number : mem_->wal_number;
        s = versions_->log_and_apply(&edit);
    }
    if (meta != nullptr) {
        pending_outputs_.erase(meta->number);
    }
    if (s.ok()) {
        imms_.erase(imms_.begin());
        stats_.flush_bytes.fetch_add(meta != nullptr ? meta->file_size : 0,
                                     std::memory_order_relaxed);
        stats_.flush_count.fetch_add(1, std::memory_order_relaxed);
    }
    return s;
}

Status DBImpl::write_level0_table(const std::shared_ptr<MemTable>& mem,
                                  std::shared_ptr<FileMeta>* meta) {
    meta->reset();
    if (mem->empty()) {
        return Status::okay();
    }
    std::uint64_t fnum;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fnum = versions_->new_file_number();
        // Stays pending until the caller's log_and_apply lands, so the file
        // GC can never reap a table that is about to become live.
        pending_outputs_.insert(fnum);
    }

    const std::string fname = table_file_name(dbname_, fnum);
    std::unique_ptr<WritableFile> file;
    Status s = env_->new_writable_file(fname, &file);
    if (!s.ok()) {
        return s;
    }
    auto out = std::make_shared<FileMeta>();
    out->number = fnum;
    {
        TableBuilder builder(options_, icmp_, file.get());
        const std::unique_ptr<Iterator> iter(mem->new_iterator());
        bool first = true;
        for (iter->seek_to_first(); iter->valid(); iter->next()) {
            if (first) {
                out->smallest.decode_from(iter->key());
                first = false;
            }
            out->largest.decode_from(iter->key());
            builder.add(iter->key(), iter->value());
        }
        s = builder.finish();
        out->file_size = builder.file_size();
    }
    // SST + its directory entry durable BEFORE the manifest references it.
    if (s.ok()) {
        s = file->sync(options_.use_fullfsync);
    }
    if (s.ok()) {
        s = file->close();
    }
    if (s.ok()) {
        s = env_->sync_dir(dbname_);
    }
    if (s.ok()) {
        *meta = std::move(out);
    }
    return s;
}

// ===========================================================================
// Compaction
// ===========================================================================

Status DBImpl::compact_all() {
    Status s = flush();
    if (!s.ok()) {
        return s;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    for (int level = 0; level < kNumLevels - 1; ++level) {
        while (true) {
            if (!bg_error_.ok()) {
                return bg_error_;
            }
            if (shutting_down_) {
                return Status::busy("shutting down");
            }
            if (versions_->current()->files[level].empty()) {
                break;
            }
            manual_compact_level_ = level;
            bg_work_cv_.notify_all();
            manual_cv_.wait(lock, [this] {
                return manual_compact_level_ == -1 || !bg_error_.ok() || shutting_down_;
            });
        }
    }
    return bg_error_;
}

Status DBImpl::compact_level_for_test(int level) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!bg_error_.ok()) {
        return bg_error_;
    }
    manual_compact_level_ = level;
    bg_work_cv_.notify_all();
    manual_cv_.wait(
        lock, [this] { return manual_compact_level_ == -1 || !bg_error_.ok() || shutting_down_; });
    return bg_error_;
}

void DBImpl::compaction_thread_main() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutting_down_) {
        CompactionJob job;
        bool has_work = false;
        if (bg_error_.ok()) {
            if (manual_compact_level_ >= 0) {
                has_work = versions_->pick_compaction_at_level(manual_compact_level_, &job);
                if (!has_work) {
                    manual_compact_level_ = -1;
                    manual_cv_.notify_all();
                }
            }
            if (!has_work) {
                has_work = versions_->pick_compaction(&job);
            }
        }
        if (!has_work) {
            bg_work_cv_.wait(lock);
            continue;
        }
        compaction_in_progress_ = true;
        const Status s = do_compaction(&job, lock);
        compaction_in_progress_ = false;
        if (!s.ok() && !shutting_down_) {
            record_background_error(s);
        }
        if (manual_compact_level_ >= 0) {
            manual_compact_level_ = -1;
        }
        manual_cv_.notify_all();
        stall_cv_.notify_all();
        remove_obsolete_files(lock);
    }
}

bool DBImpl::is_base_level_for_key(const Version& v, int first_level, const Slice& ukey,
                                   std::vector<std::size_t>* level_ptrs) const {
    // Keys arrive in ascending order, so each level's cursor only moves
    // forward across the whole compaction.
    for (int level = first_level; level < kNumLevels; ++level) {
        const auto& files = v.files[level];
        while ((*level_ptrs)[static_cast<std::size_t>(level)] < files.size()) {
            const auto& f = files[(*level_ptrs)[static_cast<std::size_t>(level)]];
            if (ukey.compare(f->largest.user_key()) <= 0) {
                if (ukey.compare(f->smallest.user_key()) >= 0) {
                    return false; // a deeper file may hold this key
                }
                break;
            }
            ++(*level_ptrs)[static_cast<std::size_t>(level)];
        }
    }
    return true;
}

Status DBImpl::do_compaction(CompactionJob* job, std::unique_lock<std::mutex>& lock) {
    const int level = job->level;

    if (job->trivial_move) {
        const auto& f = job->inputs[0][0];
        VersionEdit edit;
        edit.deleted_files.emplace_back(level, f->number);
        edit.added_files.emplace_back(level + 1, f);
        const Status s = versions_->log_and_apply(&edit);
        stats_.compaction_count.fetch_add(1, std::memory_order_relaxed);
        return s;
    }

    // Snapshot-aware GC bound: nothing visible to the oldest snapshot (or to
    // current readers) may be dropped.
    const SequenceNumber smallest_snapshot =
        snapshots_.empty() ? versions_->last_sequence() : snapshots_.oldest()->sequence;

    std::vector<std::unique_ptr<Iterator>> children;
    std::uint64_t input_bytes = 0;
    Status s;
    for (int which = 0; which < 2; ++which) {
        for (const auto& f : job->inputs[which]) {
            input_bytes += f->file_size;
            std::shared_ptr<TableReader> reader;
            s = table_cache_->find_table(f->number, f->file_size, &reader);
            if (!s.ok()) {
                return s;
            }
            children.emplace_back(reader->new_iterator());
        }
    }
    const std::unique_ptr<Iterator> input(new_merging_iterator(&icmp_, std::move(children)));

    lock.unlock();

    std::vector<std::shared_ptr<FileMeta>> outputs;
    std::unique_ptr<WritableFile> out_file;
    std::unique_ptr<TableBuilder> builder;
    std::shared_ptr<FileMeta> cur;
    std::uint64_t output_bytes = 0;

    const auto finish_output = [&]() -> Status {
        if (builder == nullptr) {
            return Status::okay();
        }
        Status fs = builder->finish();
        if (fs.ok()) {
            cur->file_size = builder->file_size();
            fs = out_file->sync(options_.use_fullfsync);
        }
        if (fs.ok()) {
            fs = out_file->close();
        }
        if (fs.ok()) {
            output_bytes += cur->file_size;
            outputs.push_back(std::move(cur));
        }
        builder.reset();
        out_file.reset();
        cur.reset();
        return fs;
    };

    std::string current_user_key;
    bool has_current_user_key = false;
    SequenceNumber last_seq_for_key = kMaxSequenceNumber;
    std::vector<std::size_t> level_ptrs(kNumLevels, 0);
    std::string prev_ikey;
    std::uint64_t processed = 0;

    for (input->seek_to_first(); s.ok() && input->valid(); input->next()) {
        if ((++processed & 4095u) == 0) {
            lock.lock();
            const bool die = shutting_down_;
            lock.unlock();
            if (die) {
                s = Status::busy("shutting down");
                break;
            }
        }
        const Slice ikey = input->key();
        ParsedInternalKey pik;
        if (!parse_internal_key(ikey, &pik)) {
            s = Status::corruption("malformed key in compaction input");
            break;
        }

        bool drop = false;
        if (!has_current_user_key || pik.user_key.compare(Slice(current_user_key)) != 0) {
            // User-key boundary: the only legal place to cut an output file
            // (a split user key would break the level invariant).
            if (builder != nullptr && builder->file_size() >= options_.target_file_size) {
                s = finish_output();
                if (!s.ok()) {
                    break;
                }
            }
            current_user_key.assign(pik.user_key.data(), pik.user_key.size());
            has_current_user_key = true;
            last_seq_for_key = kMaxSequenceNumber;
        } else if (icmp_.compare(ikey, Slice(prev_ikey)) == 0) {
            // Exact duplicate internal key: possible only from re-replayed
            // WALs after a crash during recovery. Keep the first copy.
            drop = true;
        }

        if (!drop) {
            if (last_seq_for_key <= smallest_snapshot) {
                // A newer version of this key is already at-or-below the
                // oldest snapshot: nothing can ever see this one.
                drop = true;
            } else if (pik.type == kTypeDeletion && pik.sequence <= smallest_snapshot &&
                       is_base_level_for_key(*job->base, level + 2, pik.user_key, &level_ptrs)) {
                // Tombstone with no deeper level to shadow: drop it too.
                drop = true;
            }
        }
        last_seq_for_key = pik.sequence;
        prev_ikey.assign(ikey.data(), ikey.size());
        if (drop) {
            continue;
        }

        if (builder == nullptr) {
            std::uint64_t fnum;
            {
                std::lock_guard<std::mutex> plock(mutex_);
                fnum = versions_->new_file_number();
                pending_outputs_.insert(fnum);
            }
            cur = std::make_shared<FileMeta>();
            cur->number = fnum;
            s = env_->new_writable_file(table_file_name(dbname_, fnum), &out_file);
            if (!s.ok()) {
                break;
            }
            builder = std::make_unique<TableBuilder>(options_, icmp_, out_file.get());
            cur->smallest.decode_from(ikey);
        }
        builder->add(ikey, input->value());
        cur->largest.decode_from(ikey);
    }

    if (s.ok()) {
        s = input->status();
    }
    if (s.ok()) {
        s = finish_output();
    } else {
        builder.reset();
        out_file.reset();
    }
    if (s.ok() && !outputs.empty()) {
        s = env_->sync_dir(dbname_);
    }

    lock.lock();
    if (s.ok()) {
        VersionEdit edit;
        for (int which = 0; which < 2; ++which) {
            for (const auto& f : job->inputs[which]) {
                edit.deleted_files.emplace_back(level + which, f->number);
            }
        }
        for (const auto& out : outputs) {
            edit.added_files.emplace_back(level + 1, out);
        }
        s = versions_->log_and_apply(&edit);
    }
    for (const auto& out : outputs) {
        pending_outputs_.erase(out->number);
    }
    if (cur != nullptr) {
        pending_outputs_.erase(cur->number);
    }
    stats_.compaction_bytes_read.fetch_add(input_bytes, std::memory_order_relaxed);
    stats_.compaction_bytes_written.fetch_add(output_bytes, std::memory_order_relaxed);
    stats_.compaction_count.fetch_add(1, std::memory_order_relaxed);
    return s;
}

// ===========================================================================
// Background bookkeeping
// ===========================================================================

void DBImpl::wal_sync_thread_main() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto interval = std::chrono::milliseconds(
        options_.wal_sync_interval_ms > 0 ? options_.wal_sync_interval_ms : 1);
    while (!shutting_down_) {
        bg_work_cv_.wait_for(lock, interval);
        if (shutting_down_) {
            break;
        }
        if (wal_ != nullptr && wal_unsynced_bytes_ > 0 && bg_error_.ok()) {
            const Status s = wal_->sync(options_.use_fullfsync);
            if (s.ok()) {
                wal_unsynced_bytes_ = 0;
            } else {
                record_background_error(s);
            }
        }
    }
}

void DBImpl::record_background_error(const Status& s) {
    if (bg_error_.ok() && !s.ok()) {
        bg_error_ = s;
        bg_work_cv_.notify_all();
        stall_cv_.notify_all();
        manual_cv_.notify_all();
    }
}

void DBImpl::remove_obsolete_files(std::unique_lock<std::mutex>& lock) {
    if (!bg_error_.ok()) {
        return; // keep everything for post-mortem in a broken state
    }
    std::set<std::uint64_t> live = pending_outputs_;
    versions_->add_live_files(&live);

    std::vector<std::string> children;
    if (!env_->get_children(dbname_, &children).ok()) {
        return;
    }
    const std::uint64_t min_wal = versions_->min_wal_number();

    std::vector<std::string> doomed;
    std::vector<std::uint64_t> evict;
    for (const auto& name : children) {
        std::uint64_t number = 0;
        FileType type = FileType::kUnknown;
        if (!parse_file_name(name, &number, &type)) {
            continue; // unknown files are not ours to delete
        }
        bool keep = true;
        switch (type) {
        case FileType::kWal:
            keep = number >= min_wal;
            break;
        case FileType::kTable:
            keep = live.count(number) > 0;
            break;
        case FileType::kTempManifest:
            keep = false; // stale tmp from a crashed manifest write
            break;
        case FileType::kManifest:
        case FileType::kLock:
        case FileType::kUnknown:
            keep = true;
            break;
        }
        if (!keep) {
            doomed.push_back(dbname_ + "/" + name);
            if (type == FileType::kTable) {
                evict.push_back(number);
            }
        }
    }
    for (const std::uint64_t number : evict) {
        table_cache_->evict(number);
    }
    // File numbers are never reused, so deleting outside the lock is safe.
    lock.unlock();
    for (const auto& path : doomed) {
        env_->remove_file(path);
    }
    lock.lock();
}

DbStats DBImpl::stats() const {
    DbStats out;
    out.user_bytes_written = stats_.user_bytes.load(std::memory_order_relaxed);
    out.wal_bytes_written = stats_.wal_bytes.load(std::memory_order_relaxed);
    out.flush_bytes_written = stats_.flush_bytes.load(std::memory_order_relaxed);
    out.compaction_bytes_read = stats_.compaction_bytes_read.load(std::memory_order_relaxed);
    out.compaction_bytes_written = stats_.compaction_bytes_written.load(std::memory_order_relaxed);
    out.flush_count = stats_.flush_count.load(std::memory_order_relaxed);
    out.compaction_count = stats_.compaction_count.load(std::memory_order_relaxed);
    out.write_stall_micros = stats_.stall_micros.load(std::memory_order_relaxed);
    out.bloom_checks = table_stats_.bloom_checks.load(std::memory_order_relaxed);
    out.bloom_skips = table_stats_.bloom_skips.load(std::memory_order_relaxed);
    if (block_cache_ != nullptr) {
        out.block_cache_hits = block_cache_->hits();
        out.block_cache_misses = block_cache_->misses();
    }
    return out;
}

} // namespace strata
