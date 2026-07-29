#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

// docs/DESIGN.md §1.2:
//   file   := fixed64 magic | fixed64 db_uuid | record*
//   record := fixed32 masked_crc32c(type|payload) | fixed32 len | u8 type
//           | payload[len]
inline constexpr std::uint64_t kWalMagic = 0x5354524154415731ull; // "STRATAW1"
inline constexpr std::size_t kWalFileHeaderSize = 16;
inline constexpr std::size_t kWalRecordHeaderSize = 9;

enum WalRecordType : std::uint8_t {
    kWalFullBatch = 0x01,
};

} // namespace strata
