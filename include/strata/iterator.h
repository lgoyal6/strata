#pragma once

#include "strata/slice.h"
#include "strata/status.h"

namespace strata {

// Forward-only iterator (v1 limitation, see docs/DESIGN.md §9).
// key()/value() slices are valid until the next mutation of the iterator.
class Iterator {
  public:
    Iterator() = default;
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    virtual ~Iterator() = default;

    virtual bool valid() const = 0;
    virtual void seek_to_first() = 0;
    virtual void seek(const Slice& target) = 0;
    virtual void next() = 0;
    virtual Slice key() const = 0;
    virtual Slice value() const = 0;
    virtual Status status() const = 0;
};

} // namespace strata
