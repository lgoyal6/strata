#include <atomic>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "db/memtable.h"

namespace strata {
namespace {

TEST(InternalKey, OrderingUserKeyAscSeqDesc) {
    InternalKeyComparator icmp;
    std::string a, b, c, d;
    append_internal_key(&a, "apple", 9, kTypeValue);
    append_internal_key(&b, "apple", 5, kTypeValue);
    append_internal_key(&c, "apple", 5, kTypeDeletion);
    append_internal_key(&d, "banana", 1, kTypeValue);
    EXPECT_LT(icmp.compare(Slice(a), Slice(b)), 0); // newer seq first
    EXPECT_LT(icmp.compare(Slice(b), Slice(c)), 0); // Value before Deletion at same seq
    EXPECT_LT(icmp.compare(Slice(c), Slice(d)), 0); // user key ascending
    EXPECT_EQ(icmp.compare(Slice(a), Slice(a)), 0);
}

TEST(InternalKey, ParseRejectsGarbage) {
    ParsedInternalKey pik;
    EXPECT_FALSE(parse_internal_key(Slice("short"), &pik));
    std::string k;
    append_internal_key(&k, "x", 3, kTypeValue);
    k[k.size() - 8] = 0x7f; // invalid type byte
    EXPECT_FALSE(parse_internal_key(Slice(k), &pik));
}

TEST(FileNames, ParseRoundTrip) {
    std::uint64_t number = 0;
    FileType type = FileType::kUnknown;
    ASSERT_TRUE(parse_file_name("000042.wal", &number, &type));
    EXPECT_EQ(number, 42u);
    EXPECT_EQ(type, FileType::kWal);
    ASSERT_TRUE(parse_file_name("999999.sst", &number, &type));
    EXPECT_EQ(type, FileType::kTable);
    ASSERT_TRUE(parse_file_name("MANIFEST", &number, &type));
    EXPECT_EQ(type, FileType::kManifest);
    ASSERT_TRUE(parse_file_name("MANIFEST.tmp", &number, &type));
    EXPECT_EQ(type, FileType::kTempManifest);
    EXPECT_FALSE(parse_file_name("foreign.txt", &number, &type));
    EXPECT_FALSE(parse_file_name(".DS_Store", &number, &type));
}

TEST(MemTable, VersionVisibility) {
    InternalKeyComparator icmp;
    MemTable mem(icmp);
    EXPECT_TRUE(mem.empty());
    mem.add(10, kTypeValue, "k", "v10");
    mem.add(20, kTypeValue, "k", "v20");
    mem.add(30, kTypeDeletion, "k", "");
    EXPECT_FALSE(mem.empty());

    std::string value;
    Status s;
    ASSERT_TRUE(mem.get(LookupKey("k", 15), &value, &s));
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(value, "v10");
    ASSERT_TRUE(mem.get(LookupKey("k", 25), &value, &s));
    EXPECT_EQ(value, "v20");
    ASSERT_TRUE(mem.get(LookupKey("k", 99), &value, &s));
    EXPECT_TRUE(s.is_not_found());                         // tombstone wins
    EXPECT_FALSE(mem.get(LookupKey("k", 5), &value, &s));  // nothing visible yet
    EXPECT_FALSE(mem.get(LookupKey("q", 99), &value, &s)); // absent key
}

TEST(MemTable, IteratorSortedAndComplete) {
    InternalKeyComparator icmp;
    MemTable mem(icmp);
    // Insert shuffled keys.
    for (const int i : {7, 1, 9, 3, 0, 8, 2, 6, 4, 5}) {
        char key[16];
        std::snprintf(key, sizeof(key), "key%03d", i);
        mem.add(static_cast<SequenceNumber>(100 + i), kTypeValue, key, "value");
    }
    const std::unique_ptr<Iterator> it(mem.new_iterator());
    int expect = 0;
    for (it->seek_to_first(); it->valid(); it->next()) {
        ParsedInternalKey pik;
        ASSERT_TRUE(parse_internal_key(it->key(), &pik));
        char want[16];
        std::snprintf(want, sizeof(want), "key%03d", expect);
        EXPECT_EQ(pik.user_key.to_string(), want);
        ++expect;
    }
    EXPECT_EQ(expect, 10);
}

// One writer, four lock-free readers hammering visibility. Run under
// TSan/ASan in the dev preset.
TEST(MemTable, ConcurrentReadersSeeMonotonicState) {
    InternalKeyComparator icmp;
    auto mem = std::make_shared<MemTable>(icmp);
    std::atomic<SequenceNumber> published{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const SequenceNumber upto = published.load(std::memory_order_acquire);
                if (upto == 0) {
                    continue;
                }
                // Every key with seq <= published must be visible.
                const SequenceNumber probe = 1 + upto / 2;
                std::string value;
                Status s;
                char key[24];
                std::snprintf(key, sizeof(key), "key%06llu",
                              static_cast<unsigned long long>(probe));
                ASSERT_TRUE(mem->get(LookupKey(key, upto), &value, &s));
                ASSERT_TRUE(s.ok());
                ASSERT_EQ(value, std::string("value-") + key);
            }
        });
    }
    for (SequenceNumber seq = 1; seq <= 20000; ++seq) {
        char key[24];
        std::snprintf(key, sizeof(key), "key%06llu", static_cast<unsigned long long>(seq));
        mem->add(seq, kTypeValue, key, std::string("value-") + key);
        published.store(seq, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    for (auto& r : readers) {
        r.join();
    }
}

} // namespace
} // namespace strata
