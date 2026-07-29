#include <gtest/gtest.h>

#include "db/version.h"

namespace strata {
namespace {

ManifestData sample() {
    ManifestData m;
    m.db_uuid = 0xabcdef0123456789ull;
    m.next_file_number = 42;
    m.last_sequence = 100000;
    m.min_wal_number = 17;
    for (int level = 0; level < 3; ++level) {
        for (int i = 0; i < 4; ++i) {
            ManifestData::File f;
            f.number = static_cast<std::uint64_t>(level * 10 + i);
            f.file_size = 8u << 20;
            append_internal_key(&f.smallest, "aaa" + std::to_string(i), 5, kTypeValue);
            append_internal_key(&f.largest, "zzz" + std::to_string(i), 9, kTypeValue);
            m.files[level].push_back(std::move(f));
        }
    }
    return m;
}

TEST(Manifest, RoundTrip) {
    const ManifestData in = sample();
    std::string encoded;
    encode_manifest(in, &encoded);

    ManifestData out;
    ASSERT_TRUE(parse_manifest(Slice(encoded), &out).ok());
    EXPECT_EQ(out.db_uuid, in.db_uuid);
    EXPECT_EQ(out.next_file_number, in.next_file_number);
    EXPECT_EQ(out.last_sequence, in.last_sequence);
    EXPECT_EQ(out.min_wal_number, in.min_wal_number);
    for (int level = 0; level < kNumLevels; ++level) {
        ASSERT_EQ(out.files[level].size(), in.files[level].size());
        for (std::size_t i = 0; i < in.files[level].size(); ++i) {
            EXPECT_EQ(out.files[level][i].number, in.files[level][i].number);
            EXPECT_EQ(out.files[level][i].smallest, in.files[level][i].smallest);
            EXPECT_EQ(out.files[level][i].largest, in.files[level][i].largest);
        }
    }
}

TEST(Manifest, EveryTruncationRejected) {
    std::string encoded;
    encode_manifest(sample(), &encoded);
    for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
        ManifestData out;
        EXPECT_FALSE(parse_manifest(Slice(encoded.data(), cut), &out).ok())
            << "accepted truncation at " << cut;
    }
}

TEST(Manifest, EveryByteFlipRejected) {
    std::string encoded;
    encode_manifest(sample(), &encoded);
    for (std::size_t pos = 0; pos < encoded.size(); ++pos) {
        std::string mutated = encoded;
        mutated[pos] = static_cast<char>(mutated[pos] ^ 0x01);
        ManifestData out;
        EXPECT_FALSE(parse_manifest(Slice(mutated), &out).ok()) << "accepted a bit flip at " << pos;
    }
}

TEST(Manifest, TrailingGarbageRejected) {
    std::string encoded;
    encode_manifest(sample(), &encoded);
    encoded += "extra";
    ManifestData out;
    EXPECT_FALSE(parse_manifest(Slice(encoded), &out).ok());
}

} // namespace
} // namespace strata
