#pragma once

#include <cstdint>

namespace strata {

// xorshift64* - deterministic given a seed; used for skiplist heights and
// test workloads (never for anything security-sensitive).
class Random {
  public:
    explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545f4914f6cdd1dull;
    }

    // Uniform in [0, n). n must be > 0.
    std::uint32_t uniform(std::uint32_t n) {
        return static_cast<std::uint32_t>(next() % n);
    }

    bool one_in(std::uint32_t n) {
        return uniform(n) == 0;
    }

  private:
    std::uint64_t state_;
};

} // namespace strata
