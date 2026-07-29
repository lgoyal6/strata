#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace strata {

// Sharded LRU cache for uncompressed data blocks. Keys are
// (sstable file number, block offset) packed into 64 bits; SSTables are
// immutable, so there is no invalidation — entries for deleted files simply
// age out. Values are shared_ptr so a block stays alive while any iterator
// still points into it, even after eviction.
class BlockCache {
  public:
    explicit BlockCache(std::size_t capacity_bytes);
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    // File numbers are well below 2^24 in practice; offsets below 2^40 (1 TiB).
    static std::uint64_t make_key(std::uint64_t file_number, std::uint64_t offset) {
        return (file_number << 40) | offset;
    }

    std::shared_ptr<const std::string> lookup(std::uint64_t key);
    void insert(std::uint64_t key, std::shared_ptr<const std::string> value);

    std::uint64_t hits() const {
        return hits_.load(std::memory_order_relaxed);
    }
    std::uint64_t misses() const {
        return misses_.load(std::memory_order_relaxed);
    }

  private:
    static constexpr std::size_t kNumShards = 16;

    struct Entry {
        std::uint64_t key;
        std::shared_ptr<const std::string> value;
    };

    struct Shard {
        std::mutex mu;
        std::list<Entry> lru; // front = most recently used
        std::unordered_map<std::uint64_t, std::list<Entry>::iterator> index;
        std::size_t usage = 0;
        std::size_t capacity = 0;
    };

    static std::size_t shard_of(std::uint64_t key) {
        // Fibonacci hash of the key selects the shard.
        return static_cast<std::size_t>((key * 0x9e3779b97f4a7c15ull) >> 60);
    }

    Shard shards_[kNumShards];
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

} // namespace strata
