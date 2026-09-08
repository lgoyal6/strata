// Concurrency suite for the compaction worker pool: bounded-queue
// backpressure, shutdown with queued and active work, overlapping-input
// exclusion, background-error propagation, readers and writers racing
// compaction, serial-vs-parallel determinism, and crash+reopen after
// compaction publications. Runs under ASan/UBSan and TSan (the fork-based
// crash test is skipped under TSan, which cannot follow a forked child
// that starts threads).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "db/compaction_scheduler.h"
#include "db/db_impl.h"
#include "db/table_cache.h"
#include "db/version.h"
#include "strata/db.h"
#include "test_util.h"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STRATA_TSAN 1
#endif
#endif

namespace strata {
namespace {

std::string value_for(std::uint64_t key, std::uint64_t rev) {
    std::string v = "v" + std::to_string(key) + ":" + std::to_string(rev) + ":";
    v.append(100 - std::min<std::size_t>(v.size(), 90), 'x');
    return v;
}

std::unique_ptr<DBImpl> open_with_workers(const Options& options, const std::string& dir,
                                          int workers, std::size_t queue_capacity = 8) {
    BackgroundConfig bg;
    bg.compaction_workers = workers;
    bg.compaction_queue_capacity = queue_capacity;
    auto impl = std::make_unique<DBImpl>(options, dir, bg);
    const Status s = impl->init();
    EXPECT_TRUE(s.ok()) << s.to_string();
    if (!s.ok()) {
        return nullptr;
    }
    return impl;
}

Options churn_options() {
    Options options;
    options.fsync_policy = FsyncPolicy::kNever; // unit tests: speed
    options.write_buffer_size = 32 * 1024;      // constant flush + compaction
    options.block_cache_bytes = 4u << 20;
    return options;
}

std::map<std::string, std::string> scan_all(DB* db, const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::map<std::string, std::string> out;
    const std::unique_ptr<Iterator> it(db->new_iterator(ro));
    for (it->seek_to_first(); it->valid(); it->next()) {
        out.emplace(it->key().to_string(), it->value().to_string());
    }
    EXPECT_TRUE(it->status().ok()) << it->status().to_string();
    return out;
}

// ===========================================================================
// CompactionScheduler component tests
// ===========================================================================

TEST(CompactionSchedulerTest, BoundedQueueAppliesBackpressure) {
    CompactionScheduler pool(1, 2);

    std::promise<void> release;
    std::shared_future<void> gate(release.get_future());
    std::atomic<int> ran{0};

    // Occupy the single worker.
    ASSERT_TRUE(pool.submit([gate, &ran] {
        gate.wait();
        ran.fetch_add(1);
    }));
    // The worker may not have popped it yet; wait until it does so the queue
    // capacity below is exercised by queued (not running) tasks.
    while (pool.queued_count() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Fill the queue to capacity.
    ASSERT_TRUE(pool.submit([&ran] { ran.fetch_add(1); }));
    ASSERT_TRUE(pool.submit([&ran] { ran.fetch_add(1); }));
    ASSERT_EQ(pool.queued_count(), 2u);

    // Bounded: a full queue rejects non-blocking submission.
    EXPECT_FALSE(pool.try_submit([&ran] { ran.fetch_add(1); }));

    // Backpressure: blocking submit waits for a slot instead of growing the
    // queue.
    std::atomic<bool> submitted{false};
    std::thread submitter([&] {
        ASSERT_TRUE(pool.submit([&ran] { ran.fetch_add(1); }));
        submitted.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(submitted.load()) << "submit returned while the queue was full";

    release.set_value(); // worker drains: slots open, submit unblocks
    submitter.join();
    EXPECT_TRUE(submitted.load());

    pool.shutdown();
    EXPECT_EQ(ran.load(), 4);
    EXPECT_EQ(pool.dropped_count(), 0u);
}

TEST(CompactionSchedulerTest, ShutdownCancelsQueuedAndJoinsActive) {
    auto pool = std::make_unique<CompactionScheduler>(1, 8);

    std::promise<void> release;
    std::shared_future<void> gate(release.get_future());
    std::atomic<bool> active_started{false};
    std::atomic<bool> active_finished{false};
    std::atomic<int> queued_ran{0};

    ASSERT_TRUE(pool->submit([&, gate] {
        active_started.store(true);
        gate.wait();
        active_finished.store(true);
    }));
    while (!active_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(pool->submit([&queued_ran] { queued_ran.fetch_add(1); }));
    }
    ASSERT_EQ(pool->queued_count(), 3u);

    // Let the active task finish shortly after shutdown starts waiting.
    std::thread releaser([&release] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release.set_value();
    });
    pool->shutdown();
    // Sampled at the instant shutdown() returns: a shutdown that joins its
    // workers cannot return before the active task completed, while one
    // that leaks them returns immediately (the release fires ~50 ms later).
    const bool finished_when_shutdown_returned = active_finished.load();
    releaser.join();

    // The active task ran to completion and was joined; queued tasks were
    // cancelled without running; post-shutdown submission is refused.
    EXPECT_TRUE(finished_when_shutdown_returned);
    EXPECT_EQ(queued_ran.load(), 0);
    EXPECT_EQ(pool->dropped_count(), 3u);
    EXPECT_FALSE(pool->submit([] {}));
    EXPECT_FALSE(pool->try_submit([] {}));
    pool.reset(); // destructor after explicit shutdown must be a no-op
}

// ===========================================================================
// Overlapping-input exclusion (deterministic, VersionSet level)
// ===========================================================================

TEST(CompactionExclusionTest, OverlappingInputsAreNotPickedTwice) {
    const std::string dir = test::make_temp_dir("excl");
    ASSERT_FALSE(dir.empty());

    Options options;
    options.l1_target_bytes = 1u << 20; // L1 scores above 1.0 with 4 MiB
    Env* env = Env::default_env();
    InternalKeyComparator icmp;
    TableReadStats stats;
    TableCache cache(env, dir, options, icmp, nullptr, &stats);
    VersionSet vset(env, dir, &options, &cache, &icmp);
    bool created = false;
    ASSERT_TRUE(vset.recover(&created).ok());

    const auto file = [&](const char* smallest, const char* largest,
                          std::uint64_t size) -> std::shared_ptr<FileMeta> {
        auto f = std::make_shared<FileMeta>();
        f->number = vset.new_file_number();
        f->file_size = size;
        f->smallest = InternalKey(smallest, 100, kTypeValue);
        f->largest = InternalKey(largest, 1, kTypeValue);
        return f;
    };
    // L1: two disjoint files; L2: one file spanning both their ranges.
    const auto f1 = file("a", "c", 2u << 20);
    const auto f2 = file("d", "f", 2u << 20);
    const auto g = file("a", "z", 1u << 20);
    VersionEdit edit;
    edit.added_files.emplace_back(1, f1);
    edit.added_files.emplace_back(1, f2);
    edit.added_files.emplace_back(2, g);
    ASSERT_TRUE(vset.log_and_apply(&edit).ok());

    // First pick takes f1 and its L2 parent g.
    CompactionJob job1;
    ASSERT_TRUE(vset.pick_compaction_at_level(1, {}, &job1));
    ASSERT_EQ(job1.inputs[0].size(), 1u);
    EXPECT_EQ(job1.inputs[0][0]->number, f1->number);
    ASSERT_EQ(job1.inputs[1].size(), 1u);
    EXPECT_EQ(job1.inputs[1][0]->number, g->number);

    std::set<std::uint64_t> busy;
    for (const auto& side : job1.inputs) {
        for (const auto& f : side) {
            busy.insert(f->number);
        }
    }

    // While job1 runs, f2 cannot compact: it would rewrite g concurrently.
    CompactionJob job2;
    EXPECT_FALSE(vset.pick_compaction_at_level(1, busy, &job2));
    EXPECT_FALSE(vset.pick_compaction(busy, &job2));

    // Once job1's inputs are released the same candidate is legal.
    CompactionJob job3;
    ASSERT_TRUE(vset.pick_compaction_at_level(1, {}, &job3));
    ASSERT_EQ(job3.inputs[0].size(), 1u);
    EXPECT_EQ(job3.inputs[0][0]->number, f2->number);
    ASSERT_EQ(job3.inputs[1].size(), 1u);
    EXPECT_EQ(job3.inputs[1][0]->number, g->number);

    test::destroy_dir(dir);
}

// ===========================================================================
// DB-level concurrency
// ===========================================================================

TEST(ConcurrencyTest, ConcurrentWritersAndReadersWhileCompacting) {
    const std::string dir = test::make_temp_dir("rw");
    ASSERT_FALSE(dir.empty());
    auto db = open_with_workers(churn_options(), dir, 4);
    ASSERT_NE(db, nullptr);

    constexpr int kWriters = 4;
    constexpr int kKeysPerWriter = 1200;
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};

    const auto key_of = [](int t, int i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "w%d:%05d", t, i);
        return std::string(buf);
    };

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int t = 0; t < kWriters; ++t) {
        writers.emplace_back([&, t] {
            // Pass 1: write. Pass 2: overwrite. Pass 3: delete every 7th.
            for (int i = 0; i < kKeysPerWriter; ++i) {
                if (!db->put(WriteOptions(), key_of(t, i), value_for(i, 1)).ok()) {
                    failures.fetch_add(1);
                    return;
                }
            }
            for (int i = 0; i < kKeysPerWriter; ++i) {
                if (!db->put(WriteOptions(), key_of(t, i), value_for(i, 2)).ok()) {
                    failures.fetch_add(1);
                    return;
                }
            }
            for (int i = 0; i < kKeysPerWriter; i += 7) {
                if (!db->remove(WriteOptions(), key_of(t, i)).ok()) {
                    failures.fetch_add(1);
                    return;
                }
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&, r] {
            std::mt19937 rng(1000u + static_cast<unsigned>(r));
            while (!done.load()) {
                // Point reads: any observed value must be one this key was
                // ever given (rev 1 or rev 2), never a torn or foreign one.
                const int t = static_cast<int>(rng() % kWriters);
                const int i = static_cast<int>(rng() % kKeysPerWriter);
                std::string v;
                const Status s = db->get(ReadOptions(), key_of(t, i), &v);
                if (s.ok()) {
                    if (v != value_for(i, 1) && v != value_for(i, 2)) {
                        failures.fetch_add(1);
                        return;
                    }
                } else if (!s.is_not_found()) {
                    failures.fetch_add(1);
                    return;
                }
                // Snapshot scan: keys strictly ascending, status clean.
                const Snapshot* snap = db->get_snapshot();
                ReadOptions ro;
                ro.snapshot = snap;
                const std::unique_ptr<Iterator> it(db->new_iterator(ro));
                std::string prev;
                for (it->seek_to_first(); it->valid(); it->next()) {
                    const std::string k = it->key().to_string();
                    if (!prev.empty() && k <= prev) {
                        failures.fetch_add(1);
                        break;
                    }
                    prev = std::move(k);
                }
                if (!it->status().ok()) {
                    failures.fetch_add(1);
                }
                db->release_snapshot(snap);
            }
        });
    }

    for (auto& w : writers) {
        w.join();
    }
    done.store(true);
    for (auto& r : readers) {
        r.join();
    }
    ASSERT_EQ(failures.load(), 0);

    // flush() surfaces any recorded background error.
    ASSERT_TRUE(db->flush().ok());

    std::map<std::string, std::string> expected;
    for (int t = 0; t < kWriters; ++t) {
        for (int i = 0; i < kKeysPerWriter; ++i) {
            if (i % 7 != 0) {
                expected[key_of(t, i)] = value_for(i, 2);
            }
        }
    }
    EXPECT_EQ(scan_all(db.get()), expected);

    // Full manual compaction under the pool, then recheck and reopen.
    ASSERT_TRUE(db->compact_all().ok());
    EXPECT_EQ(scan_all(db.get()), expected);
    db.reset();
    auto reopened = open_with_workers(churn_options(), dir, 1);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(scan_all(reopened.get()), expected);
    reopened.reset();
    test::destroy_dir(dir);
}

TEST(ConcurrencyTest, ShutdownWithQueuedAndActiveCompactions) {
    const std::string dir = test::make_temp_dir("shutdown");
    ASSERT_FALSE(dir.empty());
    constexpr int kKeys = 20000;
    {
        auto db = open_with_workers(churn_options(), dir, 4, 4);
        ASSERT_NE(db, nullptr);
        for (int i = 0; i < kKeys; ++i) {
            ASSERT_TRUE(db->put(WriteOptions(), "s" + std::to_string(i), value_for(i, 1)).ok());
        }
        // Destroy immediately: flushes and compactions are still queued and
        // running. The destructor must cancel queued work, join every
        // worker, and lose nothing that was acknowledged.
        db.reset();
    }
    auto db = open_with_workers(churn_options(), dir, 1);
    ASSERT_NE(db, nullptr);
    const auto contents = scan_all(db.get());
    ASSERT_EQ(contents.size(), static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        const auto it = contents.find("s" + std::to_string(i));
        ASSERT_NE(it, contents.end()) << "acknowledged key lost: " << i;
        EXPECT_EQ(it->second, value_for(i, 1));
    }
    db.reset();
    test::destroy_dir(dir);
}

// Env wrapper that can refuse to create new SSTables, simulating a full or
// failing disk during compaction output writes.
class SstFaultEnv final : public Env {
  public:
    SstFaultEnv() : base_(Env::default_env()) {}

    std::atomic<bool> fail_sst{false};

    Status new_writable_file(const std::string& f, std::unique_ptr<WritableFile>* r) override {
        if (fail_sst.load() && f.size() >= 4 && f.compare(f.size() - 4, 4, ".sst") == 0) {
            return Status::io_error("injected sst write failure");
        }
        return base_->new_writable_file(f, r);
    }
    Status new_sequential_file(const std::string& f, std::unique_ptr<SequentialFile>* r) override {
        return base_->new_sequential_file(f, r);
    }
    Status new_random_access_file(const std::string& f,
                                  std::unique_ptr<RandomAccessFile>* r) override {
        return base_->new_random_access_file(f, r);
    }
    bool file_exists(const std::string& f) override {
        return base_->file_exists(f);
    }
    Status get_children(const std::string& d, std::vector<std::string>* r) override {
        return base_->get_children(d, r);
    }
    Status remove_file(const std::string& f) override {
        return base_->remove_file(f);
    }
    Status truncate_file(const std::string& f, std::uint64_t s) override {
        return base_->truncate_file(f, s);
    }
    Status rename_file(const std::string& s, const std::string& d) override {
        return base_->rename_file(s, d);
    }
    Status create_dir_if_missing(const std::string& d) override {
        return base_->create_dir_if_missing(d);
    }
    Status get_file_size(const std::string& f, std::uint64_t* s) override {
        return base_->get_file_size(f, s);
    }
    Status sync_dir(const std::string& d) override {
        return base_->sync_dir(d);
    }
    Status lock_file(const std::string& f, FileLock** l) override {
        return base_->lock_file(f, l);
    }
    Status unlock_file(FileLock* l) override {
        return base_->unlock_file(l);
    }
    std::uint64_t now_micros() override {
        return base_->now_micros();
    }
    void sleep_micros(std::uint64_t m) override {
        base_->sleep_micros(m);
    }

  private:
    Env* const base_;
};

TEST(ConcurrencyTest, CompactionErrorPropagatesToOwner) {
    const std::string dir = test::make_temp_dir("bgerr");
    ASSERT_FALSE(dir.empty());
    SstFaultEnv fault_env;
    Options options = churn_options();
    options.env = &fault_env;

    auto db = open_with_workers(options, dir, 2);
    ASSERT_NE(db, nullptr);
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(db->put(WriteOptions(), "e" + std::to_string(i), value_for(i, 1)).ok());
    }
    ASSERT_TRUE(db->flush().ok()); // one healthy L0 file

    fault_env.fail_sst.store(true);
    const Status s = db->compact_level_for_test(0);
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.to_string().find("injected sst write failure"), std::string::npos) << s.to_string();

    // The database owner sees the background error on the write path too.
    const Status w = db->put(WriteOptions(), "after", "x");
    ASSERT_FALSE(w.ok());
    EXPECT_NE(w.to_string().find("injected sst write failure"), std::string::npos) << w.to_string();

    fault_env.fail_sst.store(false);
    db.reset();
    test::destroy_dir(dir);
}

TEST(ConcurrencyTest, SerialAndParallelReachSameFinalState) {
    const std::string dir_serial = test::make_temp_dir("det-serial");
    const std::string dir_parallel = test::make_temp_dir("det-parallel");
    ASSERT_FALSE(dir_serial.empty());
    ASSERT_FALSE(dir_parallel.empty());

    auto serial = open_with_workers(churn_options(), dir_serial, 1);
    auto parallel = open_with_workers(churn_options(), dir_parallel, 4);
    ASSERT_NE(serial, nullptr);
    ASSERT_NE(parallel, nullptr);

    std::map<std::string, std::string> model;
    std::mt19937 rng(1234);
    for (int op = 0; op < 12000; ++op) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "k%04u", static_cast<unsigned>(rng() % 1000));
        const std::string key(buf);
        if (rng() % 5 == 0) {
            ASSERT_TRUE(serial->remove(WriteOptions(), key).ok());
            ASSERT_TRUE(parallel->remove(WriteOptions(), key).ok());
            model.erase(key);
        } else {
            const std::string value = value_for(rng() % 1000, static_cast<std::uint64_t>(op));
            ASSERT_TRUE(serial->put(WriteOptions(), key, value).ok());
            ASSERT_TRUE(parallel->put(WriteOptions(), key, value).ok());
            model[key] = value;
        }
    }

    // Logical state must agree before, and after, full compaction.
    EXPECT_EQ(scan_all(serial.get()), model);
    EXPECT_EQ(scan_all(parallel.get()), model);
    ASSERT_TRUE(serial->compact_all().ok());
    ASSERT_TRUE(parallel->compact_all().ok());
    EXPECT_EQ(scan_all(serial.get()), model);
    EXPECT_EQ(scan_all(parallel.get()), model);

    serial.reset();
    parallel.reset();
    test::destroy_dir(dir_serial);
    test::destroy_dir(dir_parallel);
}

// Crash (real SIGKILL) after compaction publications, then reopen: every
// write the child acknowledged over the pipe must survive with its exact
// value. The child runs 4 compaction workers on a tiny write buffer so the
// kill lands after several compactions have published new table sets.
TEST(ConcurrencyTest, CrashAndReopenAfterCompactionPublication) {
#ifdef STRATA_TSAN
    GTEST_SKIP() << "fork+threads is unsupported under TSan; covered by ASan/release runs";
#else
    const std::string dir = test::make_temp_dir("crash");
    ASSERT_FALSE(dir.empty());

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    constexpr std::uint32_t kCompacted = 0xFFFFFFFFu;
    constexpr int kMaxKeys = 200000;

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // Child: never return into gtest.
        ::close(fds[0]);
        Options options;
        options.fsync_policy = FsyncPolicy::kNever; // SIGKILL keeps the page cache
        options.write_buffer_size = 16 * 1024;
        BackgroundConfig bg;
        bg.compaction_workers = 4;
        auto db = std::make_unique<DBImpl>(options, dir, bg);
        if (!db->init().ok()) {
            ::_exit(2);
        }
        bool announced = false;
        for (std::uint32_t i = 0; i < kMaxKeys; ++i) {
            if (!db->put(WriteOptions(), "c" + std::to_string(i), value_for(i, 1)).ok()) {
                ::_exit(2);
            }
            const std::uint32_t ack = i;
            if (::write(fds[1], &ack, sizeof(ack)) != static_cast<ssize_t>(sizeof(ack))) {
                ::_exit(2);
            }
            if (!announced && i % 512 == 0 && db->stats().compaction_count >= 3) {
                const std::uint32_t marker = kCompacted;
                if (::write(fds[1], &marker, sizeof(marker)) !=
                    static_cast<ssize_t>(sizeof(marker))) {
                    ::_exit(2);
                }
                announced = true;
            }
        }
        ::_exit(announced ? 0 : 3);
    }

    // Parent: collect acknowledgements; SIGKILL a while after the child
    // reports that compactions have published.
    ::close(fds[1]);
    std::int64_t last_acked = -1;
    bool compacted = false;
    int acks_after_compacted = 0;
    bool killed = false;
    while (true) {
        std::uint32_t v = 0;
        const ssize_t n = ::read(fds[0], &v, sizeof(v));
        if (n == 0) {
            break; // EOF: child gone (killed or finished)
        }
        ASSERT_EQ(n, static_cast<ssize_t>(sizeof(v)));
        if (v == kCompacted) {
            compacted = true;
            continue;
        }
        last_acked = v;
        if (compacted && !killed && ++acks_after_compacted >= 2000) {
            ::kill(child, SIGKILL);
            killed = true;
            // keep draining: acks already in the pipe are acknowledged writes
        }
    }
    ::close(fds[0]);
    int wstatus = 0;
    ASSERT_EQ(::waitpid(child, &wstatus, 0), child);
    if (!killed) {
        // The child finished the whole workload before compacting 3 times;
        // that would make this test vacuous, so fail loudly.
        ASSERT_TRUE(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
            << "child exited abnormally: " << wstatus;
        FAIL() << "child was never killed after compactions; workload too small";
    }
    ASSERT_TRUE(compacted);
    ASSERT_GE(last_acked, 0);

    // Reopen the killed database and verify the acknowledged prefix.
    auto db = open_with_workers(churn_options(), dir, 2);
    ASSERT_NE(db, nullptr);
    for (std::int64_t i = 0; i <= last_acked; ++i) {
        std::string v;
        const Status s =
            db->get(ReadOptions(), "c" + std::to_string(static_cast<std::uint32_t>(i)), &v);
        ASSERT_TRUE(s.ok()) << "acknowledged key " << i << " lost after crash: " << s.to_string();
        ASSERT_EQ(v, value_for(static_cast<std::uint64_t>(i), 1)) << "wrong value for key " << i;
    }
    db.reset();
    test::destroy_dir(dir);
#endif
}

} // namespace
} // namespace strata
