#pragma once

// YCSB core-workload machinery: scrambled zipfian key selection (Gray et
// al.'s algorithm, theta 0.99, exactly as in the YCSB reference
// implementation) over a hashed key space.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ycsb {

inline std::uint64_t fnv64(std::uint64_t v) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (int i = 0; i < 8; ++i) {
        hash ^= (v >> (8 * i)) & 0xffu;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

class Rng {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x853c49e6748fea9bull : seed) {}

    std::uint64_t next_u64() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    double next_double() { // [0, 1)
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    std::uint64_t uniform(std::uint64_t n) {
        return next_u64() % n;
    }

  private:
    std::uint64_t state_;
};

class ZipfianGenerator {
  public:
    ZipfianGenerator(std::uint64_t items, double theta = 0.99) : items_(items), theta_(theta) {
        zetan_ = zeta(items_);
        zeta2_ = zeta(2);
        alpha_ = 1.0 / (1.0 - theta_);
        eta_ = (1.0 - std::pow(2.0 / static_cast<double>(items_), 1.0 - theta_)) /
               (1.0 - zeta2_ / zetan_);
    }

    std::uint64_t next(Rng& rng) const {
        const double u = rng.next_double();
        const double uz = u * zetan_;
        if (uz < 1.0) {
            return 0;
        }
        if (uz < 1.0 + std::pow(0.5, theta_)) {
            return 1;
        }
        return static_cast<std::uint64_t>(static_cast<double>(items_) *
                                          std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }

    // Scrambled: hot items scatter across the key space (YCSB default).
    std::uint64_t next_scrambled(Rng& rng) const {
        return fnv64(next(rng)) % items_;
    }

  private:
    double zeta(std::uint64_t n) const {
        double sum = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i + 1), theta_);
        }
        return sum;
    }

    std::uint64_t items_;
    double theta_;
    double zetan_, zeta2_, alpha_, eta_;
};

inline std::string key_name(std::uint64_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user%016llx", static_cast<unsigned long long>(fnv64(index)));
    return buf;
}

inline std::string make_value(Rng& rng, std::size_t size) {
    std::string v;
    v.reserve(size);
    while (v.size() < size) {
        v.push_back(static_cast<char>('a' + rng.uniform(26)));
    }
    return v;
}

} // namespace ycsb
