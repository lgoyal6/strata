#pragma once

#include <cstdint>
#include <string>

#include "strata/slice.h"
#include "strata/status.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {

// docs/DESIGN.md §1.1.
inline constexpr std::uint64_t kTableMagic = 0x5354524154414231ull; // "STRATAB1"
inline constexpr std::uint32_t kTableFormatVersion = 1;
inline constexpr std::size_t kFooterSize = 56;
// Every block is followed by: u8 compression_type | fixed32 masked crc32c.
inline constexpr std::size_t kBlockTrailerSize = 5;
inline constexpr std::uint8_t kNoCompression = 0;

struct BlockHandle {
    std::uint64_t offset = 0;
    std::uint64_t size = 0; // excludes the trailer

    void encode_to(std::string* dst) const {
        put_varint64(dst, offset);
        put_varint64(dst, size);
    }

    bool decode_from(Slice* input) {
        return get_varint64(input, &offset) && get_varint64(input, &size);
    }
};

struct Footer {
    BlockHandle filter_handle;
    BlockHandle index_handle;

    void encode_to(std::string* dst) const {
        const std::size_t start = dst->size();
        filter_handle.encode_to(dst);
        index_handle.encode_to(dst);
        dst->resize(start + 40); // zero-pad the handle area
        put_fixed32(dst, kTableFormatVersion);
        const std::uint32_t crc = crc32c(dst->data() + start, 44);
        put_fixed32(dst, crc32c_mask(crc));
        put_fixed64(dst, kTableMagic);
    }

    Status decode_from(const Slice& input) {
        if (input.size() != kFooterSize) {
            return Status::corruption("bad footer size");
        }
        if (decode_fixed64(input.data() + 48) != kTableMagic) {
            return Status::corruption("bad table magic");
        }
        const std::uint32_t expected = crc32c_unmask(decode_fixed32(input.data() + 44));
        if (crc32c(input.data(), 44) != expected) {
            return Status::corruption("footer checksum mismatch");
        }
        if (decode_fixed32(input.data() + 40) != kTableFormatVersion) {
            return Status::corruption("unsupported table version");
        }
        Slice handles(input.data(), 40);
        if (!filter_handle.decode_from(&handles) || !index_handle.decode_from(&handles)) {
            return Status::corruption("bad footer handles");
        }
        return Status::okay();
    }
};

} // namespace strata
