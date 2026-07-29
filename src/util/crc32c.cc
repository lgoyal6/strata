#include "util/crc32c.h"

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace strata {
namespace {

#if !defined(__ARM_FEATURE_CRC32) && !defined(__SSE4_2__)
consteval std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0x82f63b78u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = make_table();

std::uint32_t crc_software(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    while (n-- > 0) {
        crc = kTable[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    }
    return crc;
}
#else
inline std::uint64_t load64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
#endif

} // namespace

std::uint32_t crc32c_extend(std::uint32_t init_crc, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = ~init_crc;
#if defined(__ARM_FEATURE_CRC32)
    while (n >= 8) {
        crc = __crc32cd(crc, load64(p));
        p += 8;
        n -= 8;
    }
    while (n-- > 0) {
        crc = __crc32cb(crc, *p++);
    }
#elif defined(__SSE4_2__)
    while (n >= 8) {
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, load64(p)));
        p += 8;
        n -= 8;
    }
    while (n-- > 0) {
        crc = _mm_crc32_u8(crc, *p++);
    }
#else
    crc = crc_software(crc, p, n);
#endif
    return ~crc;
}

} // namespace strata
