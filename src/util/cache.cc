#include "util/cache.h"

namespace strata {

BlockCache::BlockCache(std::size_t capacity_bytes) {
    for (auto& shard : shards_) {
        shard.capacity = capacity_bytes / kNumShards;
    }
}

std::shared_ptr<const std::string> BlockCache::lookup(std::uint64_t key) {
    Shard& shard = shards_[shard_of(key)];
    std::lock_guard<std::mutex> lock(shard.mu);
    const auto it = shard.index.find(key);
    if (it == shard.index.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second->value;
}

void BlockCache::insert(std::uint64_t key, std::shared_ptr<const std::string> value) {
    Shard& shard = shards_[shard_of(key)];
    const std::size_t charge = value ? value->size() : 0;
    std::lock_guard<std::mutex> lock(shard.mu);
    const auto it = shard.index.find(key);
    if (it != shard.index.end()) {
        shard.usage -= it->second->value->size();
        it->second->value = std::move(value);
        shard.usage += charge;
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second);
    } else {
        shard.lru.push_front(Entry{key, std::move(value)});
        shard.index[key] = shard.lru.begin();
        shard.usage += charge;
    }
    while (shard.usage > shard.capacity && !shard.lru.empty()) {
        const Entry& victim = shard.lru.back();
        shard.usage -= victim.value->size();
        shard.index.erase(victim.key);
        shard.lru.pop_back();
    }
}

} // namespace strata
