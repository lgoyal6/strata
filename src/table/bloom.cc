#include "table/bloom.h"

#include "util/coding.h"

namespace strata {
namespace {

// 64-bit FNV-1a with an avalanche finalizer (splitmix64's mixer): cheap and
// well-distributed for double hashing.
std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

} // namespace

std::uint64_t bloom_hash(const Slice& key) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < key.size(); ++i) {
        h ^= static_cast<std::uint8_t>(key[i]);
        h *= 0x100000001b3ull;
    }
    return mix64(h);
}

void bloom_build(const std::vector<std::uint64_t>& hashes, int bits_per_key, std::string* dst) {
    // k = bits_per_key * ln2, clamped to [1, 30].
    int num_probes = static_cast<int>(static_cast<double>(bits_per_key) * 0.69);
    if (num_probes < 1) {
        num_probes = 1;
    }
    if (num_probes > 30) {
        num_probes = 30;
    }

    std::size_t bits = hashes.size() * static_cast<std::size_t>(bits_per_key);
    if (bits < 64) {
        bits = 64;
    }
    const std::size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    dst->clear();
    put_fixed32(dst, static_cast<std::uint32_t>(num_probes));
    const std::size_t bits_start = dst->size();
    dst->resize(bits_start + bytes, '\0');
    char* array = dst->data() + bits_start;

    for (const std::uint64_t h : hashes) {
        const std::uint64_t delta = (h >> 33) | (h << 31);
        std::uint64_t pos = h;
        for (int i = 0; i < num_probes; ++i) {
            const std::size_t bit = static_cast<std::size_t>(pos % bits);
            array[bit / 8] =
                static_cast<char>(static_cast<std::uint8_t>(array[bit / 8]) | (1u << (bit % 8)));
            pos += delta;
        }
    }
}

bool bloom_may_contain(const Slice& filter, std::uint64_t hash) {
    if (filter.size() < 5) {
        return true; // fail open
    }
    const std::uint32_t num_probes = decode_fixed32(filter.data());
    if (num_probes == 0 || num_probes > 30) {
        return true; // fail open on nonsense (fuzzed) filters
    }
    const std::size_t bits = (filter.size() - 4) * 8;
    const char* array = filter.data() + 4;
    const std::uint64_t delta = (hash >> 33) | (hash << 31);
    std::uint64_t pos = hash;
    for (std::uint32_t i = 0; i < num_probes; ++i) {
        const std::size_t bit = static_cast<std::size_t>(pos % bits);
        if ((static_cast<std::uint8_t>(array[bit / 8]) & (1u << (bit % 8))) == 0) {
            return false;
        }
        pos += delta;
    }
    return true;
}

} // namespace strata
