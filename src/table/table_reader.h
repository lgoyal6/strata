#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "db/dbformat.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/block.h"
#include "table/format.h"
#include "util/cache.h"
#include "util/env.h"

namespace strata {

// Bloom-filter accounting shared across all readers (owned by DBImpl).
struct TableReadStats {
    std::atomic<std::uint64_t> bloom_checks{0};
    std::atomic<std::uint64_t> bloom_skips{0};
};

// Read side of one immutable SSTable. Index and filter blocks are pinned at
// open; data blocks go through the (optional) shared BlockCache.
class TableReader : public std::enable_shared_from_this<TableReader> {
  public:
    // Validates footer, index and filter (CRC-checked). block_cache and
    // stats may be null.
    static Status open(const Options& options, const InternalKeyComparator& icmp,
                       std::unique_ptr<RandomAccessFile> file, std::uint64_t file_number,
                       std::uint64_t file_size, BlockCache* block_cache, TableReadStats* stats,
                       std::shared_ptr<TableReader>* out);

    // Physical point probe: positions at the first entry >= internal_key.
    // If that entry shares internal_key's user key it is copied out and
    // *found = true (the caller interprets the tag: value vs tombstone).
    Status get(const Slice& internal_key, std::string* found_ikey, std::string* found_value,
               bool* found);

    // Iterator over the whole table (internal keys). Pins this reader.
    Iterator* new_iterator();

    std::uint64_t file_number() const {
        return file_number_;
    }

  private:
    class Iter;

    TableReader(const Options& options, const InternalKeyComparator& icmp,
                std::unique_ptr<RandomAccessFile> file, std::uint64_t file_number,
                std::uint64_t file_size, BlockCache* block_cache, TableReadStats* stats)
        : options_(options), icmp_(icmp), file_(std::move(file)), file_number_(file_number),
          file_size_(file_size), block_cache_(block_cache), stats_(stats) {}

    // Reads + CRC-verifies a block, via the cache when present.
    Status read_block(const BlockHandle& handle, std::shared_ptr<const Block>* out) const;
    // Reads + CRC-verifies raw block bytes without Block framing (filter).
    Status read_raw(const BlockHandle& handle, std::string* out) const;

    const Options options_; // by-value: reader can outlive the caller's Options
    const InternalKeyComparator icmp_;
    const std::unique_ptr<RandomAccessFile> file_;
    const std::uint64_t file_number_;
    const std::uint64_t file_size_;
    BlockCache* const block_cache_;
    TableReadStats* const stats_;

    std::shared_ptr<const Block> index_block_;
    std::string filter_; // empty when the table has no filter block
};

} // namespace strata
