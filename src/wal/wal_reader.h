#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "strata/status.h"
#include "util/env.h"

namespace strata {

// Replays a WAL segment. Stops at the first torn/corrupt record (treated as
// the tail, per docs/DESIGN.md §1.2) - a bad record is never returned.
class WalReader {
  public:
    // expected_uuid == 0 skips the UUID check (used before the MANIFEST's
    // uuid is known, e.g. by tools and fuzzers).
    static Status open(Env* env, const std::string& fname, std::uint64_t expected_uuid,
                       std::unique_ptr<WalReader>* out);

    // Returns true and fills *record with the payload of the next valid
    // record. Returns false at clean EOF or at the torn tail; after false,
    // truncated_tail() reports whether trailing bytes were discarded.
    bool read_record(std::string* record);

    bool truncated_tail() const {
        return truncated_tail_;
    }
    std::uint64_t db_uuid() const {
        return db_uuid_;
    }
    // Byte offset just past the last valid record (0 for a torn file
    // header): the length recovery truncates a torn WAL to.
    std::uint64_t valid_offset() const {
        return valid_offset_;
    }

  private:
    WalReader(std::unique_ptr<SequentialFile> file, std::uint64_t file_size)
        : file_(std::move(file)), remaining_(file_size) {}

    // Reads exactly n bytes into *out unless EOF intervenes (returns false).
    bool read_fully(std::size_t n, std::string* out);

    std::unique_ptr<SequentialFile> file_;
    std::uint64_t remaining_; // bytes left in the file after the header
    std::uint64_t db_uuid_ = 0;
    std::uint64_t valid_offset_ = 0;
    bool truncated_tail_ = false;
    bool done_ = false;
};

} // namespace strata
