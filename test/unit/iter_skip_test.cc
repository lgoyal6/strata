// Iteration over keys with long runs of superseded versions.
//
// DBIter replaces a long walk over shadowed versions with one targeted seek
// past the yielded key (kSkipRunSeekThreshold). These tests pin the observable
// behaviour of that shortcut: the visible key/value stream, tombstone
// shadowing and snapshot visibility must be byte-identical to a reference
// std::map, at run lengths well above and below the threshold.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "strata/db.h"
#include "test_util.h"

namespace strata {
namespace {

class IterSkipTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = test::make_temp_dir("iterskip");
        ASSERT_FALSE(dir_.empty());
        options_.fsync_policy = FsyncPolicy::kNever;
        options_.write_buffer_size = 64u << 10; // force several L0 files
        DB* raw = nullptr;
        const Status s = DB::open(options_, dir_, &raw);
        ASSERT_TRUE(s.ok()) << s.to_string();
        db_.reset(raw);
    }

    void TearDown() override {
        db_.reset();
        test::destroy_dir(dir_);
    }

    // Walks the whole DB and compares against the reference map.
    void expect_matches(const std::map<std::string, std::string>& expected) {
        const std::unique_ptr<Iterator> it(db_->new_iterator(ReadOptions()));
        auto ref = expected.begin();
        for (it->seek_to_first(); it->valid(); it->next()) {
            ASSERT_NE(ref, expected.end()) << "iterator yielded more keys than the model";
            EXPECT_EQ(it->key().to_string(), ref->first);
            EXPECT_EQ(it->value().to_string(), ref->second);
            ++ref;
        }
        ASSERT_TRUE(it->status().ok()) << it->status().to_string();
        EXPECT_EQ(ref, expected.end()) << "iterator stopped short of the model";
    }

    std::string dir_;
    Options options_;
    std::unique_ptr<DB> db_;
};

// One key overwritten far more times than the seek threshold, surrounded by
// neighbours that must still appear exactly once each.
TEST_F(IterSkipTest, LongVersionRunYieldsOnlyNewest) {
    std::map<std::string, std::string> model;
    // "hot\0" is the immediate successor of "hot" in bytewise order: it is the
    // one key an off-by-one in the skip target would step over.
    for (const std::string& k :
         {std::string("aaa"), std::string("hot\0", 4), std::string("mmm"), std::string("zzz")}) {
        const std::string v = "edge-" + k;
        ASSERT_TRUE(db_->put(WriteOptions(), k, v).ok());
        model[k] = v;
    }
    for (int i = 0; i < 5000; ++i) {
        const std::string v = "v" + std::to_string(i);
        ASSERT_TRUE(db_->put(WriteOptions(), "hot", v).ok());
        model["hot"] = v;
    }
    expect_matches(model);
}

// Several hot keys interleaved, each with a run crossing the threshold, so a
// seek that lands one key short or one key long would be visible.
TEST_F(IterSkipTest, InterleavedHotKeysKeepOrdering) {
    std::map<std::string, std::string> model;
    const std::vector<std::string> hot = {"k02", "k04", "k06"};
    for (int i = 0; i < 9; ++i) {
        const std::string k = "k0" + std::to_string(i);
        ASSERT_TRUE(db_->put(WriteOptions(), k, "cold").ok());
        model[k] = "cold";
    }
    for (int round = 0; round < 400; ++round) {
        for (const std::string& k : hot) {
            const std::string v = "hot" + std::to_string(round);
            ASSERT_TRUE(db_->put(WriteOptions(), k, v).ok());
            model[k] = v;
        }
    }
    expect_matches(model);
}

// A tombstone on top of a long version run must hide every older version,
// and must not swallow the following key.
TEST_F(IterSkipTest, TombstoneOverLongRunHidesWholeKey) {
    std::map<std::string, std::string> model;
    ASSERT_TRUE(db_->put(WriteOptions(), "before", "b").ok());
    model["before"] = "b";
    for (int i = 0; i < 2000; ++i) {
        ASSERT_TRUE(db_->put(WriteOptions(), "doomed", "v" + std::to_string(i)).ok());
    }
    ASSERT_TRUE(db_->remove(WriteOptions(), "doomed").ok());
    ASSERT_TRUE(db_->put(WriteOptions(), "after", "a").ok());
    model["after"] = "a";
    expect_matches(model);
}

// A snapshot taken mid-run must still see its own version, which sits under
// thousands of newer ones: the shortcut must not skip past the snapshot's
// visible entry.
TEST_F(IterSkipTest, SnapshotSeesItsOwnVersionUnderALongRun) {
    ASSERT_TRUE(db_->put(WriteOptions(), "aaa", "a0").ok());
    ASSERT_TRUE(db_->put(WriteOptions(), "hot", "pinned").ok());
    ASSERT_TRUE(db_->put(WriteOptions(), "zzz", "z0").ok());
    const Snapshot* snap = db_->get_snapshot();
    ASSERT_NE(snap, nullptr);
    for (int i = 0; i < 3000; ++i) {
        ASSERT_TRUE(db_->put(WriteOptions(), "hot", "newer" + std::to_string(i)).ok());
    }
    ReadOptions ro;
    ro.snapshot = snap;
    const std::unique_ptr<Iterator> it(db_->new_iterator(ro));
    std::vector<std::pair<std::string, std::string>> got;
    for (it->seek_to_first(); it->valid(); it->next()) {
        got.emplace_back(it->key().to_string(), it->value().to_string());
    }
    ASSERT_TRUE(it->status().ok()) << it->status().to_string();
    const std::vector<std::pair<std::string, std::string>> want = {
        {"aaa", "a0"}, {"hot", "pinned"}, {"zzz", "z0"}};
    EXPECT_EQ(got, want);
    db_->release_snapshot(snap);
}

// seek() into the middle of a long run must land on the newest visible
// version of the target, not on a shadowed one.
TEST_F(IterSkipTest, SeekIntoLongRunLandsOnNewest) {
    ASSERT_TRUE(db_->put(WriteOptions(), "aaa", "a").ok());
    for (int i = 0; i < 4000; ++i) {
        ASSERT_TRUE(db_->put(WriteOptions(), "hot", "v" + std::to_string(i)).ok());
    }
    ASSERT_TRUE(db_->put(WriteOptions(), "zzz", "z").ok());
    const std::unique_ptr<Iterator> it(db_->new_iterator(ReadOptions()));
    it->seek("hot");
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->key().to_string(), "hot");
    EXPECT_EQ(it->value().to_string(), "v3999");
    it->next();
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->key().to_string(), "zzz");
    it->next();
    EXPECT_FALSE(it->valid());
    EXPECT_TRUE(it->status().ok());
}

} // namespace
} // namespace strata
