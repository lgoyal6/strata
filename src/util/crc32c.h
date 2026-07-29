#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

// CRC32C (Castagnoli). Hardware-accelerated on ARMv8 (CRC extension) and
// x86-64 (SSE4.2), software table fallback elsewhere.
// crc32c_extend(previous_full_crc, more_data) composes incrementally.
std::uint32_t crc32c_extend(std::uint32_t init_crc, const void* data, std::size_t n);

inline std::uint32_t crc32c(const void* data, std::size_t n) {
    return crc32c_extend(0, data, n);
}

// LevelDB-style masking: stored CRCs are masked so that data containing an
// embedded CRC of itself does not checksum pathologically.
inline constexpr std::uint32_t kCrcMaskDelta = 0xa282ead8u;

inline std::uint32_t crc32c_mask(std::uint32_t crc) {
    return ((crc >> 15) | (crc << 17)) + kCrcMaskDelta;
}

inline std::uint32_t crc32c_unmask(std::uint32_t masked) {
    const std::uint32_t rot = masked - kCrcMaskDelta;
    return (rot >> 17) | (rot << 15);
}

} // namespace strata
