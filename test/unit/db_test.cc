#include <memory>

#include <gtest/gtest.h>

#include "db/db_impl.h"
#include "strata/db.h"
#include "test_util.h"

namespace strata {
namespace {

class DbTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = test::make_temp_dir("db");
        ASSERT_FALSE(dir_.empty());
        options_.fsync_policy = FsyncPolicy::kNever; // unit tests: speed
        open();
    }

    void TearDown() override {
        db_.reset();
        test::destroy_dir(dir_);
    }

    void open() {
        db_.reset();
        DB* raw = nullptr;
        const Status s = DB::open(options_, dir_, &raw);
        ASSERT_TRUE(s.ok()) << s.to_string();
        db_.reset(raw);
    }

    void reopen() {
        open();
    }

    std::string get(const std::string& key, const Snapshot* snap = nullptr) {
        ReadOptions ro;
        ro.snapshot = snap;
        std::string value;
        const Status s = db_->get(ro, key, &value);
        if (s.is_not_found()) {
            return "NOT_FOUND";
        }
        EXPECT_TRUE(s.ok()) << s.to_string();
        return value;
    }

    Status put(const std::string& k, const std::string& v) {
        return db_->put(WriteOptions(), k, v);
    }

    Status del(const std::string& k) {
        return db_->remove(WriteOptions(), k);
    }

    std::vector<std::pair<std::string, std::string>> scan(const Snapshot* snap = nullptr) {
        ReadOptions ro;
        ro.snapshot = snap;
        std::vector<std::pair<std::string, std::string>> out;
        std::unique_ptr<Iterator> it(db_->new_iterator(ro));
        for (it->seek_to_first(); it->valid(); it->next()) {
            out.emplace_back(it->key().to_string(), it->value().to_string());
        }
        EXPECT_TRUE(it->status().ok()) << it->status().to_string();
        return out;
    }

    std::string dir_;
    Options options_;
    std::unique_ptr<DB> db_;
};

TEST_F(DbTest, PutGetDeleteOverwrite) {
    ASSERT_TRUE(put("k1", "v1").ok());
    EXPECT_EQ(get("k1"), "v1");
    EXPECT_EQ(get("nope"), "NOT_FOUND");
    ASSERT_TRUE(put("k1", "v2").ok());
    EXPECT_EQ(get("k1"), "v2");
    ASSERT_TRUE(del("k1").ok());
    EXPECT_EQ(get("k1"), "NOT_FOUND");
    ASSERT_TRUE(put("k1", "v3").ok());
    EXPECT_EQ(get("k1"), "v3");
}

TEST_F(DbTest, EmptyValueAndBinaryKeys) {
    const std::string bin("\x00\x01\xff\x7f", 4);
    ASSERT_TRUE(put(bin, "").ok());
    EXPECT_EQ(get(bin), "");
    ASSERT_TRUE(put("", "empty-key").ok());
    EXPECT_EQ(get(""), "empty-key");
}

TEST_F(DbTest, AtomicWriteBatch) {
    WriteBatch batch;
    batch.put("a", "1");
    batch.put("b", "2");
    batch.remove("a");
    batch.put("c", "3");
    ASSERT_TRUE(db_->write(WriteOptions(), &batch).ok());
    EXPECT_EQ(get("a"), "NOT_FOUND");
    EXPECT_EQ(get("b"), "2");
    EXPECT_EQ(get("c"), "3");
}

TEST_F(DbTest, ReopenReplaysWal) {
    ASSERT_TRUE(put("persist", "me").ok());
    ASSERT_TRUE(del("gone").ok());
    reopen(); // no flush happened: everything must come back from the WAL
    EXPECT_EQ(get("persist"), "me");
    EXPECT_EQ(get("gone"), "NOT_FOUND");
    ASSERT_TRUE(put("more", "data").ok());
    reopen();
    EXPECT_EQ(get("persist"), "me");
    EXPECT_EQ(get("more"), "data");
}

TEST_F(DbTest, FlushMovesDataToTablesAndSurvivesReopen) {
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(put("key" + std::to_string(i), "value" + std::to_string(i)).ok());
    }
    ASSERT_TRUE(db_->flush().ok());
    EXPECT_GE(db_->stats().flush_count, 1u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(get("key" + std::to_string(i)), "value" + std::to_string(i));
    }
    reopen();
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(get("key" + std::to_string(i)), "value" + std::to_string(i));
    }
}

TEST_F(DbTest, CompactAllPreservesEverything) {
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(put("key" + std::to_string(i), std::string(100, 'v')).ok());
    }
    ASSERT_TRUE(del("key250").ok());
    ASSERT_TRUE(db_->compact_all().ok());
    EXPECT_EQ(get("key250"), "NOT_FOUND");
    EXPECT_EQ(get("key100"), std::string(100, 'v'));
    reopen();
    EXPECT_EQ(get("key250"), "NOT_FOUND");
    EXPECT_EQ(get("key499"), std::string(100, 'v'));
}

TEST_F(DbTest, SnapshotIsolationAcrossCompaction) {
    ASSERT_TRUE(put("k", "v1").ok());
    const Snapshot* snap = db_->get_snapshot();
    ASSERT_TRUE(put("k", "v2").ok());
    ASSERT_TRUE(put("other", "x").ok());

    EXPECT_EQ(get("k", snap), "v1");
    EXPECT_EQ(get("other", snap), "NOT_FOUND");
    EXPECT_EQ(get("k"), "v2");

    // Compaction must not GC versions the snapshot can still see.
    ASSERT_TRUE(db_->compact_all().ok());
    EXPECT_EQ(get("k", snap), "v1");
    EXPECT_EQ(get("k"), "v2");

    db_->release_snapshot(snap);
    ASSERT_TRUE(db_->compact_all().ok());
    EXPECT_EQ(get("k"), "v2");
}

TEST_F(DbTest, IteratorScanSortedAndTombstonesHidden) {
    ASSERT_TRUE(put("b", "2").ok());
    ASSERT_TRUE(put("a", "1").ok());
    ASSERT_TRUE(put("d", "4").ok());
    ASSERT_TRUE(put("c", "3").ok());
    ASSERT_TRUE(del("c").ok());
    const auto rows = scan();
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].first, "a");
    EXPECT_EQ(rows[1].first, "b");
    EXPECT_EQ(rows[2].first, "d");
    EXPECT_EQ(rows[2].second, "4");
}

TEST_F(DbTest, IteratorSeek) {
    for (const char* k : {"apple", "banana", "cherry", "damson"}) {
        ASSERT_TRUE(put(k, k).ok());
    }
    std::unique_ptr<Iterator> it(db_->new_iterator(ReadOptions()));
    it->seek("b");
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->key().to_string(), "banana");
    it->seek("banana");
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->key().to_string(), "banana");
    it->seek("zzz");
    EXPECT_FALSE(it->valid());
}

TEST_F(DbTest, IteratorIsStableSnapshotOfItsCreationTime) {
    ASSERT_TRUE(put("k1", "old").ok());
    std::unique_ptr<Iterator> it(db_->new_iterator(ReadOptions()));
    ASSERT_TRUE(put("k1", "new").ok());
    ASSERT_TRUE(put("k2", "invisible").ok());
    it->seek_to_first();
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->value().to_string(), "old");
    it->next();
    EXPECT_FALSE(it->valid());
}

// The tombstone-resurrection trap: value at the bottom level, tombstone
// compacted L0->L1 only. Dropping that tombstone early would resurrect the
// value underneath (docs/DESIGN.md §4).
TEST_F(DbTest, TombstoneNotDroppedAboveShadowedValue) {
    ASSERT_TRUE(put("victim", "must-stay-dead").ok());
    ASSERT_TRUE(db_->compact_all().ok()); // value now at the bottom level
    ASSERT_TRUE(del("victim").ok());
    ASSERT_TRUE(db_->flush().ok()); // tombstone in L0

    // Compact only L0 -> L1: the tombstone must survive that step.
    auto* impl = static_cast<DBImpl*>(db_.get());
    ASSERT_TRUE(impl->compact_level_for_test(0).ok());
    EXPECT_EQ(get("victim"), "NOT_FOUND");
    ASSERT_TRUE(db_->compact_all().ok());
    EXPECT_EQ(get("victim"), "NOT_FOUND");
    reopen();
    EXPECT_EQ(get("victim"), "NOT_FOUND");
}

TEST_F(DbTest, TinyBuffersPushDataThroughAllLevels) {
    db_.reset();
    test::destroy_dir(dir_);
    dir_ = test::make_temp_dir("db-tiny");
    options_.write_buffer_size = 4096;
    options_.target_file_size = 4096;
    options_.l1_target_bytes = 16384;
    options_.block_size = 512;
    open();

    for (int i = 0; i < 3000; ++i) {
        ASSERT_TRUE(put("key" + std::to_string(i % 300), "gen" + std::to_string(i / 300)).ok());
    }
    ASSERT_TRUE(db_->flush().ok());
    for (int i = 0; i < 300; ++i) {
        EXPECT_EQ(get("key" + std::to_string(i)), "gen9");
    }
    const DbStats stats = db_->stats();
    EXPECT_GE(stats.compaction_count, 1u) << "tiny buffers should force compactions";
    reopen();
    for (int i = 0; i < 300; ++i) {
        EXPECT_EQ(get("key" + std::to_string(i)), "gen9");
    }
}

TEST_F(DbTest, DoubleOpenIsRefused) {
    DB* second = nullptr;
    const Status s = DB::open(options_, dir_, &second);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(second, nullptr);
}

TEST_F(DbTest, StatsTrackWritesAndAmplification) {
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(put("key" + std::to_string(i), std::string(100, 'x')).ok());
    }
    ASSERT_TRUE(db_->flush().ok());
    const DbStats stats = db_->stats();
    EXPECT_GT(stats.user_bytes_written, 100u * 1000u);
    EXPECT_GT(stats.wal_bytes_written, stats.user_bytes_written);
    EXPECT_GT(stats.flush_bytes_written, 0u);
    EXPECT_GE(stats.write_amplification(), 2.0); // WAL + flush at minimum
}

} // namespace
} // namespace strata
