#include <map>
#include <memory>

#include <gtest/gtest.h>

#include "table/bloom.h"
#include "table/table_builder.h"
#include "table/table_reader.h"
#include "test_util.h"
#include "util/random.h"

namespace strata {
namespace {

class TableTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = test::make_temp_dir("table");
        ASSERT_FALSE(dir_.empty());
        env_ = Env::default_env();
        options_.block_size = 512; // small blocks: exercise the index hard
    }
    void TearDown() override {
        test::destroy_dir(dir_);
    }

    std::string path(int n) const {
        return dir_ + "/" + std::to_string(n) + ".sst";
    }

    // Builds a table from sorted (user_key -> value); seq descends so keys
    // stay unique.
    void build(int file_id, const std::map<std::string, std::string>& contents) {
        std::unique_ptr<WritableFile> file;
        ASSERT_TRUE(env_->new_writable_file(path(file_id), &file).ok());
        TableBuilder builder(options_, icmp_, file.get());
        SequenceNumber seq = 10'000'000; // stays far from wrapping below zero
        for (const auto& [k, v] : contents) {
            std::string ikey;
            append_internal_key(&ikey, k, seq--, kTypeValue);
            builder.add(Slice(ikey), Slice(v));
        }
        ASSERT_TRUE(builder.finish().ok());
        ASSERT_TRUE(file->sync(false).ok());
        ASSERT_TRUE(file->close().ok());
    }

    Status open(int file_id, std::shared_ptr<TableReader>* reader) {
        std::uint64_t size = 0;
        Status s = env_->get_file_size(path(file_id), &size);
        if (!s.ok()) {
            return s;
        }
        std::unique_ptr<RandomAccessFile> file;
        s = env_->new_random_access_file(path(file_id), &file);
        if (!s.ok()) {
            return s;
        }
        return TableReader::open(options_, icmp_, std::move(file),
                                 static_cast<std::uint64_t>(file_id), size, nullptr, nullptr,
                                 reader);
    }

    std::string dir_;
    Env* env_ = nullptr;
    Options options_;
    InternalKeyComparator icmp_;
};

std::map<std::string, std::string> make_contents(int n) {
    std::map<std::string, std::string> contents;
    Random rnd(42);
    for (int i = 0; i < n; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "user-key-%06d", i);
        contents[key] = std::string(rnd.uniform(150), static_cast<char>('a' + (i % 26)));
    }
    return contents;
}

TEST_F(TableTest, IterateEverythingInOrder) {
    const auto contents = make_contents(5000);
    build(1, contents);
    std::shared_ptr<TableReader> reader;
    ASSERT_TRUE(open(1, &reader).ok());

    const std::unique_ptr<Iterator> it(reader->new_iterator());
    auto expect = contents.begin();
    for (it->seek_to_first(); it->valid(); it->next(), ++expect) {
        ASSERT_NE(expect, contents.end());
        ParsedInternalKey pik;
        ASSERT_TRUE(parse_internal_key(it->key(), &pik));
        EXPECT_EQ(pik.user_key.to_string(), expect->first);
        EXPECT_EQ(it->value().to_string(), expect->second);
    }
    EXPECT_TRUE(it->status().ok());
    EXPECT_EQ(expect, contents.end());
}

TEST_F(TableTest, GetEveryKeyAndMisses) {
    const auto contents = make_contents(5000);
    build(1, contents);
    std::shared_ptr<TableReader> reader;
    ASSERT_TRUE(open(1, &reader).ok());

    for (const auto& [k, v] : contents) {
        std::string ikey;
        append_internal_key(&ikey, k, kMaxSequenceNumber, kValueTypeForSeek);
        std::string found_key, found_value;
        bool found = false;
        ASSERT_TRUE(reader->get(Slice(ikey), &found_key, &found_value, &found).ok());
        ASSERT_TRUE(found) << k;
        EXPECT_EQ(found_value, v);
    }
    for (const char* miss : {"user-key-99999x", "aaaa", "zzzz", ""}) {
        std::string ikey;
        append_internal_key(&ikey, miss, kMaxSequenceNumber, kValueTypeForSeek);
        std::string found_key, found_value;
        bool found = false;
        ASSERT_TRUE(reader->get(Slice(ikey), &found_key, &found_value, &found).ok());
        EXPECT_FALSE(found) << miss;
    }
}

TEST_F(TableTest, SeekLandsOnCeiling) {
    const auto contents = make_contents(1000);
    build(1, contents);
    std::shared_ptr<TableReader> reader;
    ASSERT_TRUE(open(1, &reader).ok());
    const std::unique_ptr<Iterator> it(reader->new_iterator());

    // Seek between keys: must land on the next key.
    std::string ikey;
    append_internal_key(&ikey, "user-key-000123a", kMaxSequenceNumber, kValueTypeForSeek);
    it->seek(Slice(ikey));
    ASSERT_TRUE(it->valid());
    ParsedInternalKey pik;
    ASSERT_TRUE(parse_internal_key(it->key(), &pik));
    EXPECT_EQ(pik.user_key.to_string(), "user-key-000124");

    // Seek past the end.
    ikey.clear();
    append_internal_key(&ikey, "zzzz", kMaxSequenceNumber, kValueTypeForSeek);
    it->seek(Slice(ikey));
    EXPECT_FALSE(it->valid());
}

TEST_F(TableTest, BloomFilterFalsePositiveRateReasonable) {
    std::vector<std::uint64_t> hashes;
    for (int i = 0; i < 10000; ++i) {
        hashes.push_back(bloom_hash(Slice("member-" + std::to_string(i))));
    }
    std::string filter;
    bloom_build(hashes, 10, &filter);
    for (int i = 0; i < 10000; ++i) {
        EXPECT_TRUE(
            bloom_may_contain(Slice(filter), bloom_hash(Slice("member-" + std::to_string(i)))));
    }
    int false_positives = 0;
    for (int i = 0; i < 10000; ++i) {
        if (bloom_may_contain(Slice(filter), bloom_hash(Slice("absent-" + std::to_string(i))))) {
            ++false_positives;
        }
    }
    // 10 bits/key targets ~1%; allow generous slack against hash unluck.
    EXPECT_LT(false_positives, 300);
}

// Flip every byte of a small table: reads must never crash and never return
// a wrong value — every flip is either detected or lands in a region whose
// bytes don't matter (there are none by design).
TEST_F(TableTest, EveryByteFlipIsDetected) {
    const auto contents = make_contents(50);
    build(1, contents);
    std::string original;
    std::uint64_t size = 0;
    ASSERT_TRUE(env_->get_file_size(path(1), &size).ok());
    {
        std::unique_ptr<SequentialFile> f;
        ASSERT_TRUE(env_->new_sequential_file(path(1), &f).ok());
        original.resize(size);
        Slice out;
        ASSERT_TRUE(f->read(size, &out, original.data()).ok());
        ASSERT_EQ(out.size(), size);
    }

    for (std::size_t pos = 0; pos < original.size(); ++pos) {
        std::string mutated = original;
        mutated[pos] = static_cast<char>(mutated[pos] ^ 0x01);
        {
            std::unique_ptr<WritableFile> f;
            ASSERT_TRUE(env_->new_writable_file(path(2), &f).ok());
            ASSERT_TRUE(f->append(Slice(mutated)).ok());
            ASSERT_TRUE(f->close().ok());
        }
        std::shared_ptr<TableReader> reader;
        const Status s = open(2, &reader);
        if (!s.ok()) {
            continue; // detected at open: good
        }
        // Openable: every successful read must still be correct.
        for (const auto& [k, v] : contents) {
            std::string ikey;
            append_internal_key(&ikey, k, kMaxSequenceNumber, kValueTypeForSeek);
            std::string found_key, found_value;
            bool found = false;
            const Status gs = reader->get(Slice(ikey), &found_key, &found_value, &found);
            if (gs.ok() && found) {
                ASSERT_EQ(found_value, v) << "byte flip at " << pos << " corrupted key " << k;
            }
        }
    }
}

} // namespace
} // namespace strata
