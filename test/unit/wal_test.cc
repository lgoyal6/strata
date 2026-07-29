#include <gtest/gtest.h>

#include "db/write_batch_internal.h"
#include "strata/write_batch.h"
#include "test_util.h"
#include "wal/wal_format.h"
#include "wal/wal_reader.h"
#include "wal/wal_writer.h"

namespace strata {
namespace {

class WalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = test::make_temp_dir("wal");
        ASSERT_FALSE(dir_.empty());
        env_ = Env::default_env();
    }
    void TearDown() override {
        test::destroy_dir(dir_);
    }

    std::string path() const {
        return dir_ + "/000001.wal";
    }

    void write_records(const std::vector<std::string>& payloads) {
        std::unique_ptr<WalWriter> w;
        ASSERT_TRUE(WalWriter::create(env_, path(), kUuid, &w).ok());
        for (const auto& p : payloads) {
            ASSERT_TRUE(w->add_record(Slice(p)).ok());
        }
        ASSERT_TRUE(w->sync(false).ok());
    }

    std::vector<std::string> read_records(bool* torn = nullptr) {
        std::unique_ptr<WalReader> r;
        const Status s = WalReader::open(env_, path(), kUuid, &r);
        EXPECT_TRUE(s.ok()) << s.to_string();
        std::vector<std::string> out;
        std::string rec;
        while (r->read_record(&rec)) {
            out.push_back(rec);
        }
        if (torn != nullptr) {
            *torn = r->truncated_tail();
        }
        return out;
    }

    static constexpr std::uint64_t kUuid = 0x1122334455667788ull;
    std::string dir_;
    Env* env_ = nullptr;
};

TEST_F(WalTest, RoundTrip) {
    write_records({"alpha", "", std::string(100000, 'x')});
    bool torn = false;
    const auto records = read_records(&torn);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0], "alpha");
    EXPECT_EQ(records[1], "");
    EXPECT_EQ(records[2], std::string(100000, 'x'));
    EXPECT_FALSE(torn);
}

TEST_F(WalTest, UuidMismatchRejected) {
    write_records({"alpha"});
    std::unique_ptr<WalReader> r;
    const Status s = WalReader::open(env_, path(), kUuid + 1, &r);
    EXPECT_TRUE(s.is_corruption()) << s.to_string();
}

TEST_F(WalTest, BadMagicRejected) {
    write_records({"alpha"});
    // Flip a byte inside the magic.
    std::string contents;
    {
        std::uint64_t size = 0;
        ASSERT_TRUE(env_->get_file_size(path(), &size).ok());
        std::unique_ptr<SequentialFile> f;
        ASSERT_TRUE(env_->new_sequential_file(path(), &f).ok());
        contents.resize(size);
        Slice out;
        ASSERT_TRUE(f->read(size, &out, contents.data()).ok());
    }
    contents[3] ^= 0x40;
    {
        std::unique_ptr<WritableFile> f;
        ASSERT_TRUE(env_->new_writable_file(path(), &f).ok());
        ASSERT_TRUE(f->append(Slice(contents)).ok());
        ASSERT_TRUE(f->close().ok());
    }
    std::unique_ptr<WalReader> r;
    EXPECT_TRUE(WalReader::open(env_, path(), kUuid, &r).is_corruption());
}

// The core torn-write property: truncate the log at EVERY byte offset and
// verify recovery yields an exact record prefix — never garbage, never a
// partial record.
TEST_F(WalTest, TruncateAtEveryByteYieldsExactPrefix) {
    const std::vector<std::string> payloads = {"first-record", "second-record",
                                               std::string(300, 'z'), "tail"};
    write_records(payloads);
    std::string full;
    std::uint64_t size = 0;
    ASSERT_TRUE(env_->get_file_size(path(), &size).ok());
    {
        std::unique_ptr<SequentialFile> f;
        ASSERT_TRUE(env_->new_sequential_file(path(), &f).ok());
        full.resize(size);
        Slice out;
        ASSERT_TRUE(f->read(size, &out, full.data()).ok());
        ASSERT_EQ(out.size(), size);
    }

    // Record boundaries: header(16) then per record 9 + payload.
    std::vector<std::size_t> boundaries = {kWalFileHeaderSize};
    for (const auto& p : payloads) {
        boundaries.push_back(boundaries.back() + kWalRecordHeaderSize + p.size());
    }

    const std::string cut_path = dir_ + "/000002.wal";
    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        {
            std::unique_ptr<WritableFile> f;
            ASSERT_TRUE(env_->new_writable_file(cut_path, &f).ok());
            ASSERT_TRUE(f->append(Slice(full.data(), cut)).ok());
            ASSERT_TRUE(f->close().ok());
        }
        std::unique_ptr<WalReader> r;
        const Status s = WalReader::open(env_, cut_path, kUuid, &r);
        if (cut < kWalFileHeaderSize) {
            // Torn inside the file header: no records, but must open cleanly.
            ASSERT_TRUE(s.ok()) << "cut=" << cut << ": " << s.to_string();
            std::string rec;
            EXPECT_FALSE(r->read_record(&rec)) << "cut=" << cut;
            continue;
        }
        ASSERT_TRUE(s.ok()) << "cut=" << cut << ": " << s.to_string();
        std::vector<std::string> got;
        std::string rec;
        while (r->read_record(&rec)) {
            got.push_back(rec);
        }
        // Expected: the number of whole records that fit under `cut`.
        std::size_t expect = 0;
        while (expect + 1 < boundaries.size() && boundaries[expect + 1] <= cut) {
            ++expect;
        }
        ASSERT_EQ(got.size(), expect) << "cut=" << cut;
        for (std::size_t i = 0; i < expect; ++i) {
            EXPECT_EQ(got[i], payloads[i]) << "cut=" << cut;
        }
        EXPECT_EQ(r->truncated_tail(), cut != boundaries[expect]) << "cut=" << cut;
    }
}

// A CRC-corrupt record mid-file stops replay without surfacing garbage.
TEST_F(WalTest, CorruptRecordStopsReplay) {
    write_records({"aaaa", "bbbb", "cccc"});
    std::string full;
    std::uint64_t size = 0;
    ASSERT_TRUE(env_->get_file_size(path(), &size).ok());
    {
        std::unique_ptr<SequentialFile> f;
        ASSERT_TRUE(env_->new_sequential_file(path(), &f).ok());
        full.resize(size);
        Slice out;
        ASSERT_TRUE(f->read(size, &out, full.data()).ok());
    }
    // Corrupt one payload byte of the second record.
    const std::size_t second_payload =
        kWalFileHeaderSize + kWalRecordHeaderSize + 4 + kWalRecordHeaderSize;
    full[second_payload + 1] ^= 0x01;
    {
        std::unique_ptr<WritableFile> f;
        ASSERT_TRUE(env_->new_writable_file(path(), &f).ok());
        ASSERT_TRUE(f->append(Slice(full)).ok());
        ASSERT_TRUE(f->close().ok());
    }
    bool torn = false;
    const auto records = read_records(&torn);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0], "aaaa");
    EXPECT_TRUE(torn);
}

TEST_F(WalTest, WriteBatchPayloadRoundTrip) {
    WriteBatch b;
    b.put("k1", "v1");
    b.remove("k2");
    b.put("k3", std::string(5000, 'q'));
    WriteBatchInternal::set_sequence(&b, 777);
    EXPECT_EQ(b.user_bytes(), 2 + 2 + 2 + 2 + 5000u);

    write_records({WriteBatchInternal::contents(&b).to_string()});
    const auto records = read_records();
    ASSERT_EQ(records.size(), 1u);
    ASSERT_TRUE(WriteBatchInternal::check(Slice(records[0])).ok());
    WriteBatch got;
    WriteBatchInternal::set_contents(&got, Slice(records[0]));
    EXPECT_EQ(WriteBatchInternal::sequence(&got), 777u);
    EXPECT_EQ(got.count(), 3u);
}

} // namespace
} // namespace strata
