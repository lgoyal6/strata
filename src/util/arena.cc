#include "util/arena.h"

#include <cassert>

namespace strata {

Arena::Arena() = default;

char* Arena::allocate(std::size_t bytes) {
    assert(bytes > 0);
    if (bytes <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }
    return allocate_fallback(bytes);
}

char* Arena::allocate_aligned(std::size_t bytes) {
    constexpr std::size_t kAlign = alignof(void*);
    static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of two");
    const std::size_t current_mod = reinterpret_cast<std::uintptr_t>(alloc_ptr_) & (kAlign - 1);
    const std::size_t slop = current_mod == 0 ? 0 : kAlign - current_mod;
    const std::size_t needed = bytes + slop;
    if (needed <= alloc_bytes_remaining_) {
        char* result = alloc_ptr_ + slop;
        alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
        return result;
    }
    // Fallback blocks from operator new[] are already max-aligned.
    return allocate_fallback(bytes);
}

char* Arena::allocate_fallback(std::size_t bytes) {
    if (bytes > kBlockSize / 4) {
        // Large objects get their own block so we never waste more than a
        // quarter of a standard block to fragmentation.
        return allocate_new_block(bytes);
    }
    char* block = allocate_new_block(kBlockSize);
    alloc_ptr_ = block + bytes;
    alloc_bytes_remaining_ = kBlockSize - bytes;
    return block;
}

char* Arena::allocate_new_block(std::size_t block_bytes) {
    auto block = std::make_unique<char[]>(block_bytes);
    char* result = block.get();
    blocks_.push_back(std::move(block));
    memory_usage_.fetch_add(block_bytes + sizeof(void*), std::memory_order_relaxed);
    return result;
}

} // namespace strata
