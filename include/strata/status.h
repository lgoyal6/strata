#pragma once

#include <cstdint>
#include <string>

#include "strata/slice.h"

namespace strata {

// No exceptions cross the public API: every fallible call returns a Status.
class Status {
  public:
    enum class Code : std::uint8_t {
        kOk = 0,
        kNotFound,
        kCorruption,
        kIoError,
        kInvalidArgument,
        kBusy,
    };

    Status() : code_(Code::kOk) {}

    static Status okay() {
        return Status();
    }
    static Status not_found(const Slice& msg = Slice()) {
        return Status(Code::kNotFound, msg);
    }
    static Status corruption(const Slice& msg) {
        return Status(Code::kCorruption, msg);
    }
    static Status io_error(const Slice& msg) {
        return Status(Code::kIoError, msg);
    }
    static Status invalid_argument(const Slice& msg) {
        return Status(Code::kInvalidArgument, msg);
    }
    static Status busy(const Slice& msg) {
        return Status(Code::kBusy, msg);
    }

    bool ok() const {
        return code_ == Code::kOk;
    }
    bool is_not_found() const {
        return code_ == Code::kNotFound;
    }
    bool is_corruption() const {
        return code_ == Code::kCorruption;
    }
    bool is_io_error() const {
        return code_ == Code::kIoError;
    }
    Code code() const {
        return code_;
    }

    std::string to_string() const;

  private:
    Status(Code code, const Slice& msg) : code_(code), msg_(msg.data(), msg.size()) {}

    Code code_;
    std::string msg_;
};

} // namespace strata
