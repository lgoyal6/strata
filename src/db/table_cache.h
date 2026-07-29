#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "db/dbformat.h"
#include "strata/options.h"
#include "strata/status.h"
#include "table/table_reader.h"
#include "util/cache.h"
#include "util/env.h"

namespace strata {

// Maps live file numbers to open TableReaders. Level counts bound the live
// file set to a few hundred, so no LRU: entries are evicted explicitly when
// the file becomes obsolete. shared_ptr keeps a reader (and its pinned
// index/filter) alive for iterators that outlive the eviction.
class TableCache {
  public:
    TableCache(Env* env, std::string dbname, const Options& options,
               const InternalKeyComparator& icmp, BlockCache* block_cache, TableReadStats* stats)
        : env_(env), dbname_(std::move(dbname)), options_(options), icmp_(icmp),
          block_cache_(block_cache), stats_(stats) {}

    Status find_table(std::uint64_t file_number, std::uint64_t file_size,
                      std::shared_ptr<TableReader>* reader);

    void evict(std::uint64_t file_number);

  private:
    Env* const env_;
    const std::string dbname_;
    const Options options_;
    const InternalKeyComparator icmp_;
    BlockCache* const block_cache_;
    TableReadStats* const stats_;

    std::mutex mu_;
    std::unordered_map<std::uint64_t, std::shared_ptr<TableReader>> readers_;
};

} // namespace strata
