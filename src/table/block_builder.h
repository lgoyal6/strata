#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "strata/slice.h"

namespace strata {

// Builds a prefix-compressed block (docs/DESIGN.md §1.1): entries share a
// prefix with their predecessor; every restart_interval-th entry stores the
// full key and its offset lands in the restart array.
class BlockBuilder {
  public:
    explicit BlockBuilder(int restart_interval);
    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    // REQUIRES: keys added in strictly increasing order; finish() not called.
    void add(const Slice& key, const Slice& value);

    // Appends the restart array + count; the returned slice is valid until
    // reset().
    Slice finish();

    void reset();

    std::size_t current_size_estimate() const {
        return buffer_.size() + restarts_.size() * 4 + 4;
    }

    bool empty() const {
        return buffer_.empty();
    }

  private:
    const int restart_interval_;
    std::string buffer_;
    std::vector<std::uint32_t> restarts_;
    int counter_ = 0;
    bool finished_ = false;
    std::string last_key_;
};

} // namespace strata
