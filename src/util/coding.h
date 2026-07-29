#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "strata/slice.h"

namespace strata {

// Little-endian fixed-width and LEB128 varint encoding (endian-independent
// implementations; the wire format is LE regardless of host order).

inline void encode_fixed32(char* dst, std::uint32_t v) {
    auto* p = reinterpret_cast<std::uint8_t*>(dst);
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline void encode_fixed64(char* dst, std::uint64_t v) {
    auto* p = reinterpret_cast<std::uint8_t*>(dst);
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

inline std::uint32_t decode_fixed32(const char* src) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(src);
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t decode_fixed64(const char* src) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(src);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

void put_fixed32(std::string* dst, std::uint32_t v);
void put_fixed64(std::string* dst, std::uint64_t v);

// Returns a pointer just past the last written byte. Buffers must be at
// least 5 (varint32) / 10 (varint64) bytes.
char* encode_varint32(char* dst, std::uint32_t v);
char* encode_varint64(char* dst, std::uint64_t v);
void put_varint32(std::string* dst, std::uint32_t v);
void put_varint64(std::string* dst, std::uint64_t v);
void put_length_prefixed_slice(std::string* dst, const Slice& value);

// Bounded parsers: return nullptr on malformed/truncated input (never read
// past `limit`). The Slice versions advance the input on success.
const char* get_varint32_ptr(const char* p, const char* limit, std::uint32_t* value);
const char* get_varint64_ptr(const char* p, const char* limit, std::uint64_t* value);
bool get_varint32(Slice* input, std::uint32_t* value);
bool get_varint64(Slice* input, std::uint64_t* value);
bool get_length_prefixed_slice(Slice* input, Slice* result);

int varint_length(std::uint64_t v);

} // namespace strata
