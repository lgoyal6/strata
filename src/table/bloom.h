#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "strata/slice.h"

namespace strata {

// Whole-file Bloom filter over user keys (docs/DESIGN.md §1.1). Callers
// hash keys once with bloom_hash and reuse the hash for both build and probe.
//   filter := fixed32 num_probes | bit array
std::uint64_t bloom_hash(const Slice& key);

void bloom_build(const std::vector<std::uint64_t>& hashes, int bits_per_key, std::string* dst);

// True if the key may be present; false only if definitely absent. An
// undersized/garbage filter returns true (fail open — correctness never
// depends on the filter).
bool bloom_may_contain(const Slice& filter, std::uint64_t hash);

} // namespace strata
