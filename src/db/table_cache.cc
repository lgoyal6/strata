#include "db/table_cache.h"

namespace strata {

Status TableCache::find_table(std::uint64_t file_number, std::uint64_t file_size,
                              std::shared_ptr<TableReader>* reader) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = readers_.find(file_number);
        if (it != readers_.end()) {
            *reader = it->second;
            return Status::okay();
        }
    }
    // Open outside the lock; a racing duplicate open is harmless (one wins).
    std::unique_ptr<RandomAccessFile> file;
    Status s = env_->new_random_access_file(table_file_name(dbname_, file_number), &file);
    if (!s.ok()) {
        return s;
    }
    std::shared_ptr<TableReader> opened;
    s = TableReader::open(options_, icmp_, std::move(file), file_number, file_size, block_cache_,
                          stats_, &opened);
    if (!s.ok()) {
        return s;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = readers_.emplace(file_number, std::move(opened));
    *reader = it->second;
    return Status::okay();
}

void TableCache::evict(std::uint64_t file_number) {
    std::lock_guard<std::mutex> lock(mu_);
    readers_.erase(file_number);
}

} // namespace strata
