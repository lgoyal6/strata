#pragma once

#include <cstdint>
#include <string>

#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

// A batch of updates applied atomically: the serialized form is exactly the
// WAL record payload (docs/DESIGN.md §1.2), so commit is a buffer append.
//   rep := fixed64 first_sequence | fixed32 count
//        | count * ( uint8 op | varint32 klen | key [ | varint32 vlen | value ] )
class WriteBatch {
  public:
    WriteBatch();

    void put(const Slice& key, const Slice& value);
    void remove(const Slice& key); // writes a tombstone
    void clear();

    std::uint32_t count() const;
    std::size_t approximate_size() const {
        return rep_.size();
    }
    // Exact key+value payload bytes (write-amplification denominator).
    std::size_t user_bytes() const {
        return user_bytes_;
    }

  private:
    friend struct WriteBatchInternal;
    std::string rep_;
    std::size_t user_bytes_ = 0;
};

} // namespace strata
