#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/block_builder.h"
#include "table/format.h"
#include "util/env.h"

namespace strata {

// Writes an SSTable (docs/DESIGN.md §1.1). Caller syncs + closes the file
// after finish() - the durability ordering lives in the flush/compaction
// code, not here.
class TableBuilder {
  public:
    TableBuilder(const Options& options, const InternalKeyComparator& icmp, WritableFile* file);
    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    // REQUIRES: internal keys added in strictly increasing icmp order.
    void add(const Slice& internal_key, const Slice& value);

    Status finish(); // filter block, index block, footer

    Status status() const {
        return status_;
    }
    std::uint64_t num_entries() const {
        return num_entries_;
    }
    // Total bytes written; exact only after finish().
    std::uint64_t file_size() const {
        return offset_;
    }

  private:
    void flush_data_block();
    void write_raw_block(const Slice& contents, BlockHandle* handle);

    const Options& options_;
    const InternalKeyComparator& icmp_;
    WritableFile* file_;

    BlockBuilder data_block_;
    BlockBuilder index_block_;
    std::vector<std::uint64_t> filter_hashes_;

    std::string last_key_;
    std::uint64_t num_entries_ = 0;
    std::uint64_t offset_ = 0;
    bool pending_index_entry_ = false;
    BlockHandle pending_handle_;
    Status status_;
};

} // namespace strata
