#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace strata {

// Bump allocator backing one memtable: allocations are freed all at once
// when the owning MemTable is destroyed. Not thread-safe for allocation
// (writes are serialized by the commit path); memory_usage() is safe to
// read concurrently.
class Arena {
  public:
    Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    char* allocate(std::size_t bytes);
    char* allocate_aligned(std::size_t bytes); // aligned for pointer-sized loads

    std::size_t memory_usage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

  private:
    char* allocate_fallback(std::size_t bytes);
    char* allocate_new_block(std::size_t block_bytes);

    static constexpr std::size_t kBlockSize = 4096;

    char* alloc_ptr_ = nullptr;
    std::size_t alloc_bytes_remaining_ = 0;
    std::vector<std::unique_ptr<char[]>> blocks_;
    std::atomic<std::size_t> memory_usage_{0};
};

} // namespace strata
