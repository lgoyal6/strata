#include "util/coding.h"

namespace strata {

void put_fixed32(std::string* dst, std::uint32_t v) {
    char buf[4];
    encode_fixed32(buf, v);
    dst->append(buf, sizeof(buf));
}

void put_fixed64(std::string* dst, std::uint64_t v) {
    char buf[8];
    encode_fixed64(buf, v);
    dst->append(buf, sizeof(buf));
}

char* encode_varint32(char* dst, std::uint32_t v) {
    auto* p = reinterpret_cast<std::uint8_t*>(dst);
    while (v >= 0x80u) {
        *p++ = static_cast<std::uint8_t>(v | 0x80u);
        v >>= 7;
    }
    *p++ = static_cast<std::uint8_t>(v);
    return reinterpret_cast<char*>(p);
}

char* encode_varint64(char* dst, std::uint64_t v) {
    auto* p = reinterpret_cast<std::uint8_t*>(dst);
    while (v >= 0x80u) {
        *p++ = static_cast<std::uint8_t>(v | 0x80u);
        v >>= 7;
    }
    *p++ = static_cast<std::uint8_t>(v);
    return reinterpret_cast<char*>(p);
}

void put_varint32(std::string* dst, std::uint32_t v) {
    char buf[5];
    dst->append(buf, static_cast<std::size_t>(encode_varint32(buf, v) - buf));
}

void put_varint64(std::string* dst, std::uint64_t v) {
    char buf[10];
    dst->append(buf, static_cast<std::size_t>(encode_varint64(buf, v) - buf));
}

void put_length_prefixed_slice(std::string* dst, const Slice& value) {
    put_varint32(dst, static_cast<std::uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

const char* get_varint32_ptr(const char* p, const char* limit, std::uint32_t* value) {
    std::uint32_t result = 0;
    for (std::uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
        const std::uint32_t byte = static_cast<std::uint8_t>(*p);
        ++p;
        if (byte & 0x80u) {
            result |= (byte & 0x7fu) << shift;
        } else {
            result |= byte << shift;
            *value = result;
            return p;
        }
    }
    return nullptr;
}

const char* get_varint64_ptr(const char* p, const char* limit, std::uint64_t* value) {
    std::uint64_t result = 0;
    for (std::uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        const std::uint64_t byte = static_cast<std::uint8_t>(*p);
        ++p;
        if (byte & 0x80u) {
            result |= (byte & 0x7fu) << shift;
        } else {
            result |= byte << shift;
            *value = result;
            return p;
        }
    }
    return nullptr;
}

bool get_varint32(Slice* input, std::uint32_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = get_varint32_ptr(p, limit, value);
    if (q == nullptr) {
        return false;
    }
    *input = Slice(q, static_cast<std::size_t>(limit - q));
    return true;
}

bool get_varint64(Slice* input, std::uint64_t* value) {
    const char* p = input->data();
    const char* limit = p + input->size();
    const char* q = get_varint64_ptr(p, limit, value);
    if (q == nullptr) {
        return false;
    }
    *input = Slice(q, static_cast<std::size_t>(limit - q));
    return true;
}

bool get_length_prefixed_slice(Slice* input, Slice* result) {
    std::uint32_t len = 0;
    if (!get_varint32(input, &len) || input->size() < len) {
        return false;
    }
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
}

int varint_length(std::uint64_t v) {
    int len = 1;
    while (v >= 0x80u) {
        v >>= 7;
        ++len;
    }
    return len;
}

} // namespace strata
