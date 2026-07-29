#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

class SequentialFile {
  public:
    virtual ~SequentialFile() = default;
    // Reads up to n bytes. *result points into scratch (caller-owned, >= n).
    virtual Status read(std::size_t n, Slice* result, char* scratch) = 0;
};

class RandomAccessFile {
  public:
    virtual ~RandomAccessFile() = default;
    virtual Status read(std::uint64_t offset, std::size_t n, Slice* result,
                        char* scratch) const = 0;
};

class WritableFile {
  public:
    virtual ~WritableFile() = default;
    virtual Status append(const Slice& data) = 0;
    // Pushes the userspace buffer into the kernel (write(2)). After flush()
    // returns, the data survives SIGKILL; sync() is only about power loss.
    virtual Status flush() = 0;
    virtual Status sync(bool full_fsync) = 0; // flush + fsync (or F_FULLFSYNC)
    virtual Status close() = 0;
};

class FileLock {
  public:
    FileLock() = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    virtual ~FileLock() = default;
};

// Filesystem abstraction. The default (POSIX) implementation contains the
// crash-fault-injection choke point used by tools/crash_test: when the
// environment variable STRATA_CRASH_AT_BYTES=<n> is set, the write(2) that
// crosses cumulative byte n (across all files) is torn at exactly that byte
// and the process raises SIGKILL. See docs/DESIGN.md §6.
class Env {
  public:
    static Env* default_env();

    Env() = default;
    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    virtual ~Env() = default;

    virtual Status new_sequential_file(const std::string& fname,
                                       std::unique_ptr<SequentialFile>* result) = 0;
    virtual Status new_random_access_file(const std::string& fname,
                                          std::unique_ptr<RandomAccessFile>* result) = 0;
    virtual Status new_writable_file(const std::string& fname,
                                     std::unique_ptr<WritableFile>* result) = 0;

    virtual bool file_exists(const std::string& fname) = 0;
    virtual Status get_children(const std::string& dir, std::vector<std::string>* result) = 0;
    virtual Status remove_file(const std::string& fname) = 0;
    // Durably shrinks a file (ftruncate + fsync). Recovery uses this to cut
    // a torn WAL tail so the tear cannot resurface as a mid-sequence error
    // after further crashes (docs/DESIGN.md §1.3).
    virtual Status truncate_file(const std::string& fname, std::uint64_t size) = 0;
    virtual Status rename_file(const std::string& src, const std::string& dst) = 0;
    virtual Status create_dir_if_missing(const std::string& dirname) = 0;
    virtual Status get_file_size(const std::string& fname, std::uint64_t* size) = 0;
    virtual Status sync_dir(const std::string& dirname) = 0;

    virtual Status lock_file(const std::string& fname, FileLock** lock) = 0;
    virtual Status unlock_file(FileLock* lock) = 0;

    virtual std::uint64_t now_micros() = 0;
    virtual void sleep_micros(std::uint64_t micros) = 0;
};

} // namespace strata
