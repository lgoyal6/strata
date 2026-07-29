#include "util/env.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace strata {
namespace {

Status posix_error(const std::string& context, int err) {
    const std::string msg = context + ": " + std::strerror(err);
    if (err == ENOENT) {
        return Status::not_found(msg);
    }
    return Status::io_error(msg);
}

// ---------------------------------------------------------------------------
// Crash fault injection (docs/DESIGN.md §6). Active only when
// STRATA_CRASH_AT_BYTES is set in the environment; zero overhead otherwise
// beyond one predictable branch per buffer flush.
// ---------------------------------------------------------------------------
struct FaultState {
    long long crash_at = -1;
    std::atomic<long long> written{0};

    FaultState() {
        if (const char* e = std::getenv("STRATA_CRASH_AT_BYTES")) {
            crash_at = std::atoll(e);
        }
    }
};

FaultState g_fault;

// Writes all n bytes (retrying EINTR / short writes). Returns 0 or errno.
int write_fully(int fd, const char* p, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return 0;
}

// The choke point: every buffered byte headed for write(2) passes through
// here. If this write crosses the configured kill offset, the prefix up to
// that offset is written (a real torn write) and the process is SIGKILLed.
int checked_write(int fd, const char* p, std::size_t n) {
    if (g_fault.crash_at >= 0) {
        const long long before =
            g_fault.written.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
        const long long allowed = g_fault.crash_at - before;
        if (allowed < static_cast<long long>(n)) {
            if (allowed > 0) {
                write_fully(fd, p, static_cast<std::size_t>(allowed));
            }
            ::raise(SIGKILL);
            ::_exit(137); // unreachable
        }
    }
    return write_fully(fd, p, n);
}

// ---------------------------------------------------------------------------

class PosixSequentialFile final : public SequentialFile {
  public:
    PosixSequentialFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
    ~PosixSequentialFile() override {
        ::close(fd_);
    }

    Status read(std::size_t n, Slice* result, char* scratch) override {
        while (true) {
            const ssize_t r = ::read(fd_, scratch, n);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return posix_error(name_, errno);
            }
            *result = Slice(scratch, static_cast<std::size_t>(r));
            return Status::okay();
        }
    }

  private:
    std::string name_;
    int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
  public:
    PosixRandomAccessFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
    ~PosixRandomAccessFile() override {
        ::close(fd_);
    }

    Status read(std::uint64_t offset, std::size_t n, Slice* result, char* scratch) const override {
        const ssize_t r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
        if (r < 0) {
            return posix_error(name_, errno);
        }
        *result = Slice(scratch, static_cast<std::size_t>(r));
        return Status::okay();
    }

  private:
    std::string name_;
    int fd_;
};

class PosixWritableFile final : public WritableFile {
  public:
    PosixWritableFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}

    ~PosixWritableFile() override {
        if (fd_ >= 0) {
            close();
        }
    }

    Status append(const Slice& data) override {
        const char* p = data.data();
        std::size_t n = data.size();
        while (n > 0) {
            const std::size_t room = kBufSize - pos_;
            const std::size_t take = n < room ? n : room;
            std::memcpy(buf_ + pos_, p, take);
            pos_ += take;
            p += take;
            n -= take;
            if (pos_ == kBufSize) {
                const Status s = flush();
                if (!s.ok()) {
                    return s;
                }
            }
        }
        return Status::okay();
    }

    Status flush() override {
        if (pos_ == 0) {
            return Status::okay();
        }
        const int err = checked_write(fd_, buf_, pos_);
        pos_ = 0;
        if (err != 0) {
            return posix_error(name_, err);
        }
        return Status::okay();
    }

    Status sync(bool full_fsync) override {
        const Status s = flush();
        if (!s.ok()) {
            return s;
        }
#if defined(__APPLE__)
        if (full_fsync) {
            if (::fcntl(fd_, F_FULLFSYNC) < 0) {
                return posix_error(name_, errno);
            }
            return Status::okay();
        }
#else
        (void)full_fsync;
#endif
        if (::fsync(fd_) < 0) {
            return posix_error(name_, errno);
        }
        return Status::okay();
    }

    Status close() override {
        const Status s = flush();
        if (::close(fd_) < 0 && s.ok()) {
            const int err = errno;
            fd_ = -1;
            return posix_error(name_, err);
        }
        fd_ = -1;
        return s;
    }

  private:
    static constexpr std::size_t kBufSize = 64 * 1024;

    std::string name_;
    int fd_;
    std::size_t pos_ = 0;
    char buf_[kBufSize];
};

class PosixFileLock final : public FileLock {
  public:
    PosixFileLock(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
    ~PosixFileLock() override {
        if (fd_ >= 0) {
            ::close(fd_); // closing drops the flock
        }
    }
    int fd() const {
        return fd_;
    }
    void release_fd() {
        fd_ = -1;
    }
    const std::string& name() const {
        return name_;
    }

  private:
    std::string name_;
    int fd_;
};

class PosixEnv final : public Env {
  public:
    Status new_sequential_file(const std::string& fname,
                               std::unique_ptr<SequentialFile>* result) override {
        const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return posix_error(fname, errno);
        }
        *result = std::make_unique<PosixSequentialFile>(fname, fd);
        return Status::okay();
    }

    Status new_random_access_file(const std::string& fname,
                                  std::unique_ptr<RandomAccessFile>* result) override {
        const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return posix_error(fname, errno);
        }
        *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
        return Status::okay();
    }

    Status new_writable_file(const std::string& fname,
                             std::unique_ptr<WritableFile>* result) override {
        const int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            return posix_error(fname, errno);
        }
        *result = std::make_unique<PosixWritableFile>(fname, fd);
        return Status::okay();
    }

    bool file_exists(const std::string& fname) override {
        return ::access(fname.c_str(), F_OK) == 0;
    }

    Status get_children(const std::string& dir, std::vector<std::string>* result) override {
        result->clear();
        DIR* d = ::opendir(dir.c_str());
        if (d == nullptr) {
            return posix_error(dir, errno);
        }
        while (const dirent* entry = ::readdir(d)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
                result->push_back(name);
            }
        }
        ::closedir(d);
        return Status::okay();
    }

    Status remove_file(const std::string& fname) override {
        if (::unlink(fname.c_str()) < 0) {
            return posix_error(fname, errno);
        }
        return Status::okay();
    }

    Status truncate_file(const std::string& fname, std::uint64_t size) override {
        const int fd = ::open(fname.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            return posix_error(fname, errno);
        }
        Status s;
        if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
            s = posix_error(fname, errno);
        } else if (::fsync(fd) < 0) {
            s = posix_error(fname, errno);
        }
        ::close(fd);
        return s;
    }

    Status rename_file(const std::string& src, const std::string& dst) override {
        if (::rename(src.c_str(), dst.c_str()) < 0) {
            return posix_error(src, errno);
        }
        return Status::okay();
    }

    Status create_dir_if_missing(const std::string& dirname) override {
        if (::mkdir(dirname.c_str(), 0755) < 0 && errno != EEXIST) {
            return posix_error(dirname, errno);
        }
        return Status::okay();
    }

    Status get_file_size(const std::string& fname, std::uint64_t* size) override {
        struct stat st{};
        if (::stat(fname.c_str(), &st) < 0) {
            *size = 0;
            return posix_error(fname, errno);
        }
        *size = static_cast<std::uint64_t>(st.st_size);
        return Status::okay();
    }

    Status sync_dir(const std::string& dirname) override {
        const int fd = ::open(dirname.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return posix_error(dirname, errno);
        }
        Status s;
        if (::fsync(fd) < 0) {
            s = posix_error(dirname, errno);
        }
        ::close(fd);
        return s;
    }

    Status lock_file(const std::string& fname, FileLock** lock) override {
        *lock = nullptr;
        const int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            return posix_error(fname, errno);
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
            const int err = errno;
            ::close(fd);
            if (err == EWOULDBLOCK) {
                return Status::busy(fname + ": already locked by another process");
            }
            return posix_error(fname, err);
        }
        *lock = new PosixFileLock(fname, fd);
        return Status::okay();
    }

    Status unlock_file(FileLock* lock) override {
        delete lock; // destructor closes the fd, releasing the flock
        return Status::okay();
    }

    std::uint64_t now_micros() override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

    void sleep_micros(std::uint64_t micros) override {
        std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }
};

} // namespace

Env* Env::default_env() {
    static PosixEnv env;
    return &env;
}

} // namespace strata
