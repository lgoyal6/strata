#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "strata/slice.h"
#include "strata/status.h"
#include "util/env.h"

namespace strata {

class WalWriter {
  public:
    // Creates fname (truncating) and writes the file header. The header is
    // pushed to the kernel but not synced; the first synced record carries
    // it down.
    static Status create(Env* env, const std::string& fname, std::uint64_t db_uuid,
                         std::unique_ptr<WalWriter>* out);

    // Appends one checksummed record and flushes it to the kernel: after
    // add_record returns, the record survives SIGKILL regardless of fsync
    // policy. Durability against power loss additionally needs sync().
    Status add_record(const Slice& payload);

    // add_record runs with the DB mutex RELEASED (group-commit leader), and
    // the interval-fsync tick calls sync() concurrently — mu_ keeps the
    // WritableFile's buffer single-writer. A sync must never observe (and
    // flush) a half-appended record.
    Status sync(bool full_fsync) {
        std::lock_guard<std::mutex> lock(mu_);
        return file_->sync(full_fsync);
    }

    std::uint64_t bytes_appended() const {
        return bytes_appended_;
    }

  private:
    explicit WalWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}

    std::mutex mu_;
    std::unique_ptr<WritableFile> file_;
    std::uint64_t bytes_appended_ = 0;
};

} // namespace strata
