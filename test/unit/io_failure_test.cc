#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "strata/db.h"
#include "strata/options.h"
#include "test_util.h"
#include "util/env.h"
#include "wal/wal_writer.h"

namespace strata {
namespace {

enum class FailurePoint { kNone, kAppend, kFlush, kSync, kRename, kRemove, kSyncDir };

class TempDir {
  public:
    explicit TempDir(const std::string& tag) : path_(test::make_temp_dir(tag)) {}
    ~TempDir() {
        test::destroy_dir(path_);
    }
    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
};

struct FaultState {
    std::atomic<FailurePoint> next{FailurePoint::kNone};
    std::atomic<int> failures{0};
    std::string name_filter;
    std::function<void(FailurePoint, bool)> boundary;

    bool take(FailurePoint point, const std::string& name = {}) {
        if (!name_filter.empty() && name.find(name_filter) == std::string::npos)
            return false;
        FailurePoint expected = point;
        const bool fail = next.compare_exchange_strong(expected, FailurePoint::kNone);
        if (fail)
            failures.fetch_add(1);
        return fail;
    }
};

class FailingWritableFile final : public WritableFile {
  public:
    FailingWritableFile(std::unique_ptr<WritableFile> inner, FaultState* fault, std::string name)
        : inner_(std::move(inner)), fault_(fault), name_(std::move(name)) {}

    Status append(const Slice& data) override {
        if (fault_->take(FailurePoint::kAppend, name_))
            return Status::io_error("injected append failure");
        return inner_->append(data);
    }
    Status flush() override {
        if (fault_->take(FailurePoint::kFlush, name_))
            return Status::io_error("injected flush failure");
        return inner_->flush();
    }
    Status sync(bool full_fsync) override {
        if (fault_->take(FailurePoint::kSync, name_))
            return Status::io_error("injected sync failure");
        return inner_->sync(full_fsync);
    }
    Status close() override {
        return inner_->close();
    }

  private:
    std::unique_ptr<WritableFile> inner_;
    FaultState* fault_;
    std::string name_;
};

class FailingEnv final : public Env {
  public:
    explicit FailingEnv(FaultState* fault) : fault_(fault), inner_(Env::default_env()) {}

    Status new_sequential_file(const std::string& name,
                               std::unique_ptr<SequentialFile>* out) override {
        return inner_->new_sequential_file(name, out);
    }
    Status new_random_access_file(const std::string& name,
                                  std::unique_ptr<RandomAccessFile>* out) override {
        return inner_->new_random_access_file(name, out);
    }
    Status new_writable_file(const std::string& name, std::unique_ptr<WritableFile>* out) override {
        std::unique_ptr<WritableFile> file;
        Status s = inner_->new_writable_file(name, &file);
        if (!s.ok())
            return s;
        *out = std::make_unique<FailingWritableFile>(std::move(file), fault_, name);
        return Status::okay();
    }
    bool file_exists(const std::string& name) override {
        return inner_->file_exists(name);
    }
    Status get_children(const std::string& name, std::vector<std::string>* out) override {
        return inner_->get_children(name, out);
    }
    Status remove_file(const std::string& name) override {
        if (fault_->take(FailurePoint::kRemove))
            return Status::io_error("injected remove failure");
        if (fault_->boundary)
            fault_->boundary(FailurePoint::kRemove, false);
        Status result = inner_->remove_file(name);
        if (result.ok() && fault_->boundary)
            fault_->boundary(FailurePoint::kRemove, true);
        return result;
    }
    Status truncate_file(const std::string& name, std::uint64_t size) override {
        return inner_->truncate_file(name, size);
    }
    Status rename_file(const std::string& src, const std::string& dst) override {
        if (fault_->take(FailurePoint::kRename))
            return Status::io_error("injected rename failure");
        if (fault_->boundary)
            fault_->boundary(FailurePoint::kRename, false);
        Status result = inner_->rename_file(src, dst);
        if (result.ok() && fault_->boundary)
            fault_->boundary(FailurePoint::kRename, true);
        return result;
    }
    Status create_dir_if_missing(const std::string& name) override {
        return inner_->create_dir_if_missing(name);
    }
    Status get_file_size(const std::string& name, std::uint64_t* size) override {
        return inner_->get_file_size(name, size);
    }
    Status sync_dir(const std::string& name) override {
        if (fault_->take(FailurePoint::kSyncDir))
            return Status::io_error("injected directory sync failure");
        return inner_->sync_dir(name);
    }
    Status lock_file(const std::string& name, FileLock** lock) override {
        return inner_->lock_file(name, lock);
    }
    Status unlock_file(FileLock* lock) override {
        return inner_->unlock_file(lock);
    }
    std::uint64_t now_micros() override {
        return inner_->now_micros();
    }
    void sleep_micros(std::uint64_t micros) override {
        inner_->sleep_micros(micros);
    }

  private:
    FaultState* fault_;
    Env* inner_;
};

TEST(WalIoFailureTest, AppendFailureIsReturnedToCaller) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("wal-append-failure");
    std::unique_ptr<WalWriter> wal;
    ASSERT_TRUE(WalWriter::create(&env, dir.path() + "/000001.wal", 7, &wal).ok());
    fault.next = FailurePoint::kAppend;
    EXPECT_FALSE(wal->add_record(Slice("record")).ok());
}

TEST(WalIoFailureTest, FlushFailureIsReturnedToCaller) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("wal-flush-failure");
    std::unique_ptr<WalWriter> wal;
    ASSERT_TRUE(WalWriter::create(&env, dir.path() + "/000001.wal", 7, &wal).ok());
    fault.next = FailurePoint::kFlush;
    EXPECT_FALSE(wal->add_record(Slice("record")).ok());
}

TEST(WalIoFailureTest, SyncFailureIsReturnedToCaller) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("wal-sync-failure");
    std::unique_ptr<WalWriter> wal;
    ASSERT_TRUE(WalWriter::create(&env, dir.path() + "/000001.wal", 7, &wal).ok());
    ASSERT_TRUE(wal->add_record(Slice("record")).ok());
    fault.next = FailurePoint::kSync;
    EXPECT_FALSE(wal->sync(false).ok());
}

TEST(DbIoFailureTest, ManifestRenameFailureDoesNotLoseAcknowledgedWrite) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("db-rename-failure");
    Options options;
    options.env = &env;
    options.fsync_policy = FsyncPolicy::kNever;
    DB* raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    std::unique_ptr<DB> db(raw);
    ASSERT_TRUE(db->put(WriteOptions(), "durable", "value").ok());

    fault.next = FailurePoint::kRename;
    EXPECT_FALSE(db->flush().ok());
    db.reset();

    raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    db.reset(raw);
    std::string value;
    ASSERT_TRUE(db->get(ReadOptions(), "durable", &value).ok());
    EXPECT_EQ(value, "value");
    EXPECT_EQ(fault.failures.load(), 1);
}

TEST(DbIoFailureTest, ManifestDirectorySyncFailureKeepsAcknowledgedDataRecoverable) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("db-dir-sync-failure");
    Options options;
    options.env = &env;
    options.fsync_policy = FsyncPolicy::kNever;
    DB* raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    std::unique_ptr<DB> db(raw);
    ASSERT_TRUE(db->put(WriteOptions(), "durable", "value").ok());

    fault.next = FailurePoint::kSyncDir;
    EXPECT_FALSE(db->flush().ok());
    db.reset();

    raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    db.reset(raw);
    std::string value;
    ASSERT_TRUE(db->get(ReadOptions(), "durable", &value).ok());
    EXPECT_EQ(value, "value");
    EXPECT_EQ(fault.failures.load(), 1);
}

TEST(DbIoFailureTest, ManifestFileSyncFailureKeepsAcknowledgedDataRecoverable) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("db-manifest-sync-failure");
    Options options;
    options.env = &env;
    options.fsync_policy = FsyncPolicy::kNever;
    DB* raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    std::unique_ptr<DB> db(raw);
    ASSERT_TRUE(db->put(WriteOptions(), "durable", "value").ok());

    fault.name_filter = "MANIFEST.tmp";
    fault.next = FailurePoint::kSync;
    EXPECT_FALSE(db->flush().ok());
    db.reset();

    raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    db.reset(raw);
    std::string value;
    ASSERT_TRUE(db->get(ReadOptions(), "durable", &value).ok());
    EXPECT_EQ(value, "value");
    EXPECT_EQ(fault.failures.load(), 1);
}

TEST(DbIoFailureTest, CompactionManifestInstallFailureKeepsOldVersionReadable) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("db-compact-install-failure");
    Options options;
    options.env = &env;
    options.fsync_policy = FsyncPolicy::kNever;
    DB* raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    std::unique_ptr<DB> db(raw);
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(db->put(WriteOptions(), "key" + std::to_string(i), "value").ok());
    }
    ASSERT_TRUE(db->flush().ok());

    fault.next = FailurePoint::kRename;
    EXPECT_FALSE(db->compact_all().ok());
    std::string value;
    ASSERT_TRUE(db->get(ReadOptions(), "key17", &value).ok());
    EXPECT_EQ(value, "value");
    db.reset();

    raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    db.reset(raw);
    ASSERT_TRUE(db->get(ReadOptions(), "key17", &value).ok());
    EXPECT_EQ(value, "value");
}

TEST(DbIoFailureTest, ObsoleteFileRemovalFailureDoesNotChangeLiveData) {
    FaultState fault;
    FailingEnv env(&fault);
    TempDir dir("db-remove-failure");
    Options options;
    options.env = &env;
    options.fsync_policy = FsyncPolicy::kNever;
    DB* raw = nullptr;
    ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
    std::unique_ptr<DB> db(raw);
    ASSERT_TRUE(db->put(WriteOptions(), "durable", "old").ok());
    ASSERT_TRUE(db->flush().ok());
    ASSERT_TRUE(db->put(WriteOptions(), "durable", "new").ok());
    ASSERT_TRUE(db->flush().ok());

    fault.next = FailurePoint::kRemove;
    ASSERT_TRUE(db->compact_all().ok());
    EXPECT_EQ(fault.failures.load(), 1);
    std::string value;
    ASSERT_TRUE(db->get(ReadOptions(), "durable", &value).ok());
    EXPECT_EQ(value, "new");
}

TEST(DbIoFailureTest, ProcessKillAtCompactionInstallAndDeletionKeepsSyncedWrites) {
    for (const auto point : {FailurePoint::kRename, FailurePoint::kRemove}) {
        for (const bool after : {false, true}) {
            TempDir dir("compaction-process-boundary");
            const pid_t child = ::fork();
            ASSERT_GE(child, 0);
            if (child == 0) {
                FaultState fault;
                FailingEnv env(&fault);
                Options options;
                options.env = &env;
                options.fsync_policy = FsyncPolicy::kAlways;
                options.l0_compaction_trigger = 100;
                DB* raw = nullptr;
                if (!DB::open(options, dir.path(), &raw).ok())
                    ::_exit(71);
                std::unique_ptr<DB> db(raw);
                for (const auto value : {"old", "new"}) {
                    if (!db->put(WriteOptions(), "acknowledged", value).ok() || !db->flush().ok())
                        ::_exit(72);
                }
                fault.boundary = [point, after](FailurePoint current, bool completed) {
                    if (point == current && completed == after)
                        ::raise(SIGKILL);
                };
                db->compact_all();
                ::_exit(73); // the named boundary must actually be reached
            }
            int status = 0;
            ASSERT_EQ(::waitpid(child, &status, 0), child);
            ASSERT_TRUE(WIFSIGNALED(status)) << "boundary not hit, status=" << status;
            ASSERT_EQ(WTERMSIG(status), SIGKILL);
            Options options;
            DB* raw = nullptr;
            ASSERT_TRUE(DB::open(options, dir.path(), &raw).ok());
            std::unique_ptr<DB> recovered(raw);
            std::string value;
            ASSERT_TRUE(recovered->get(ReadOptions(), "acknowledged", &value).ok());
            EXPECT_EQ(value, "new");
        }
    }
}

TEST(DbIoFailureTest, RealDiskFullRefusesWriteAndRetainsSyncedData) {
#if defined(__linux__)
    const char* root = std::getenv("STRATA_DISK_FULL_TEST_DIR");
    if (root == nullptr)
        GTEST_SKIP() << "requires an owned size-capped tmpfs";
    struct statvfs capacity{};
    ASSERT_EQ(::statvfs(root, &capacity), 0);
    // Refuse to fill an ordinary host filesystem or a large shared tmpfs.
    ASSERT_LE(static_cast<unsigned long long>(capacity.f_blocks) * capacity.f_frsize, 32ull << 20);
    std::string pattern = std::string(root) + "/strata-enospc-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    struct Cleanup {
        std::string dir;
        ~Cleanup() {
            test::destroy_dir(dir);
        }
    } cleanup{pattern};
    DB* raw = nullptr;
    ASSERT_TRUE(DB::open(Options(), pattern, &raw).ok());
    std::unique_ptr<DB> db(raw);
    ASSERT_TRUE(db->put(WriteOptions(), "synced", "retained").ok());
    ASSERT_TRUE(db->flush().ok());
    const std::string filler = pattern + "/owned-filler";
    const int fd = ::open(filler.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    ASSERT_GE(fd, 0);
    const std::string block(4096, 'x');
    int observed_errno = 0;
    for (;;) {
        const ssize_t written = ::write(fd, block.data(), block.size());
        if (written < 0) {
            if (errno == EINTR)
                continue;
            observed_errno = errno;
            break;
        }
        if (written == 0)
            break;
    }
    ::close(fd);
    ASSERT_EQ(observed_errno, ENOSPC);
    const Status failed = db->put(WriteOptions(), "unacknowledged", std::string(8192, 'y'));
    EXPECT_FALSE(failed.ok());
    EXPECT_NE(failed.to_string().find("No space left"), std::string::npos);
    ASSERT_EQ(::unlink(filler.c_str()), 0);
    db.reset();
    raw = nullptr;
    ASSERT_TRUE(DB::open(Options(), pattern, &raw).ok());
    db.reset(raw);
    std::string value;
    ASSERT_TRUE(db->get(ReadOptions(), "synced", &value).ok());
    EXPECT_EQ(value, "retained");
#else
    GTEST_SKIP() << "owned Linux tmpfs fixture required";
#endif
}

} // namespace
} // namespace strata
