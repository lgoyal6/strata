#include <gtest/gtest.h>

#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {
namespace {

TEST(Crc32c, KnownAnswerVectors) {
    // RFC 3720 test vector.
    EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
    // 32 zero bytes.
    const char zeros[32] = {};
    EXPECT_EQ(crc32c(zeros, sizeof(zeros)), 0x8A9136AAu);
    EXPECT_EQ(crc32c("", 0), 0u);
}

TEST(Crc32c, ExtendComposes) {
    const std::uint32_t whole = crc32c("hello world", 11);
    std::uint32_t part = crc32c("hello ", 6);
    part = crc32c_extend(part, "world", 5);
    EXPECT_EQ(part, whole);
}

TEST(Crc32c, MaskRoundTrip) {
    for (const std::uint32_t v : {0u, 1u, 0xdeadbeefu, 0xffffffffu}) {
        EXPECT_EQ(crc32c_unmask(crc32c_mask(v)), v);
        EXPECT_NE(crc32c_mask(v), v);
    }
}

TEST(Coding, FixedRoundTrip) {
    std::string s;
    put_fixed32(&s, 0x01020304u);
    put_fixed64(&s, 0x0102030405060708ull);
    EXPECT_EQ(decode_fixed32(s.data()), 0x01020304u);
    EXPECT_EQ(decode_fixed64(s.data() + 4), 0x0102030405060708ull);
    // Wire format is little-endian.
    EXPECT_EQ(static_cast<unsigned char>(s[0]), 0x04);
}

TEST(Coding, VarintRoundTripBoundaries) {
    for (const std::uint64_t v : {0ull, 1ull, 127ull, 128ull, 16383ull, 16384ull, (1ull << 21) - 1,
                                  1ull << 21, (1ull << 32) - 1, 1ull << 32, (1ull << 63), ~0ull}) {
        std::string s;
        put_varint64(&s, v);
        EXPECT_EQ(static_cast<int>(s.size()), varint_length(v));
        Slice in(s);
        std::uint64_t got = 0;
        ASSERT_TRUE(get_varint64(&in, &got));
        EXPECT_EQ(got, v);
        EXPECT_TRUE(in.empty());
    }
}

TEST(Coding, VarintRejectsTruncation) {
    std::string s;
    put_varint64(&s, ~0ull);
    for (std::size_t cut = 0; cut < s.size(); ++cut) {
        Slice in(s.data(), cut);
        std::uint64_t got = 0;
        EXPECT_FALSE(get_varint64(&in, &got)) << "accepted a truncated varint at " << cut;
    }
}

TEST(Coding, VarintRejectsOverlong) {
    // Eleven continuation bytes can never terminate a varint64.
    const std::string evil(11, '\x80');
    Slice in(evil);
    std::uint64_t got = 0;
    EXPECT_FALSE(get_varint64(&in, &got));
}

TEST(Coding, LengthPrefixedSlice) {
    std::string s;
    put_length_prefixed_slice(&s, "abc");
    put_length_prefixed_slice(&s, "");
    Slice in(s), out;
    ASSERT_TRUE(get_length_prefixed_slice(&in, &out));
    EXPECT_EQ(out.to_string(), "abc");
    ASSERT_TRUE(get_length_prefixed_slice(&in, &out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(get_length_prefixed_slice(&in, &out));
}

} // namespace
} // namespace strata
