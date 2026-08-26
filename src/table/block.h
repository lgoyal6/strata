#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "db/dbformat.h"
#include "strata/iterator.h"

namespace strata {

// Read side of a prefix-compressed block. Contents are CRC-verified before
// construction, but the parser still treats every byte as adversarial
// (bounds-checked varints, restart offsets validated) - fuzz_sstable feeds
// arbitrary bytes through here.
class Block {
  public:
    // contents excludes the 5-byte trailer.
    explicit Block(std::shared_ptr<const std::string> contents);

    // The iterator shares ownership of the block, so cache eviction can
    // never free bytes an iterator still points at.
    static Iterator* new_iterator(std::shared_ptr<const Block> block,
                                  const InternalKeyComparator* cmp);

    bool malformed() const {
        return malformed_;
    }

  private:
    class Iter;

    const char* data() const {
        return contents_->data();
    }

    std::shared_ptr<const std::string> contents_;
    std::uint32_t restarts_offset_ = 0; // where the restart array begins
    std::uint32_t num_restarts_ = 0;
    bool malformed_ = false;
};

} // namespace strata
