// Model-based random tester: every operation is mirrored into a std::map;
// point gets, full scans, snapshot views, and reopen/recovery are compared
// against the model continuously. Tiny buffers force data through
// memtable -> immutable -> L0 -> deeper levels, so this exercises the WAL,
// flush, compaction GC, and iterator visibility logic together.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "strata/db.h"
#include "test_util.h"
#include "util/random.h"

namespace strata {
namespace {

class ModelTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = test::make_temp_dir("model");
        ASSERT_FALSE(dir_.empty());
        options_.write_buffer_size = 4096; // force constant flushes
        options_.target_file_size = 8192;
        options_.l1_target_bytes = 32768;
        options_.block_size = 512;
        options_.block_cache_bytes = 2048; // thrash the cache on purpose
        options_.fsync_policy = FsyncPolicy::kNever;
        open();
    }

    void TearDown() override {
        for (auto& [snap, view] : snapshots_) {
            db_->release_snapshot(snap);
        }
        snapshots_.clear();
        db_.reset();
        test::destroy_dir(dir_);
    }

    void open() {
        DB* raw = nullptr;
        const Status s = DB::open(options_, dir_, &raw);
        ASSERT_TRUE(s.ok()) << s.to_string();
        db_.reset(raw);
    }

    void reopen() {
        // Snapshots do not survive restarts; settle them first.
        verify_snapshots();
        for (auto& [snap, view] : snapshots_) {
            db_->release_snapshot(snap);
        }
        snapshots_.clear();
        // Stats are per-instance: accumulate before tearing down.
        const DbStats stats = db_->stats();
        total_flushes_ += stats.flush_count;
        total_compactions_ += stats.compaction_count;
        db_.reset();
        open();
    }

    void verify_against(const std::map<std::string, std::string>& model, const Snapshot* snap,
                        const std::string& context) {
        ReadOptions ro;
        ro.snapshot = snap;
        // Full scan must equal the model exactly.
        std::unique_ptr<Iterator> it(db_->new_iterator(ro));
        auto expect = model.begin();
        std::size_t n = 0;
        for (it->seek_to_first(); it->valid(); it->next(), ++expect, ++n) {
            ASSERT_NE(expect, model.end())
                << context << ": scan has extra key " << it->key().to_string();
            ASSERT_EQ(it->key().to_string(), expect->first) << context << " at row " << n;
            ASSERT_EQ(it->value().to_string(), expect->second)
                << context << " key " << expect->first;
        }
        ASSERT_TRUE(it->status().ok()) << it->status().to_string();
        ASSERT_EQ(expect, model.end()) << context << ": scan is missing keys";
    }

    void verify_point_gets(const std::map<std::string, std::string>& model, Random& rnd) {
        for (int i = 0; i < 64; ++i) {
            const std::string key = key_for(rnd.uniform(kKeySpace + 50));
            std::string value;
            const Status s = db_->get(ReadOptions(), key, &value);
            const auto it = model.find(key);
            if (it == model.end()) {
                ASSERT_TRUE(s.is_not_found()) << key << ": " << s.to_string();
            } else {
                ASSERT_TRUE(s.ok()) << key << ": " << s.to_string();
                ASSERT_EQ(value, it->second) << key;
            }
        }
    }

    void verify_snapshots() {
        for (auto& [snap, view] : snapshots_) {
            verify_against(view, snap, "snapshot view");
        }
    }

    static std::string key_for(std::uint32_t i) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "key%06u", i);
        return buf;
    }

    static constexpr std::uint32_t kKeySpace = 400;

    std::string dir_;
    Options options_;
    std::unique_ptr<DB> db_;
    std::vector<std::pair<const Snapshot*, std::map<std::string, std::string>>> snapshots_;
    std::uint64_t total_flushes_ = 0;
    std::uint64_t total_compactions_ = 0;
};

TEST_F(ModelTest, RandomOpsMatchReferenceModel) {
    std::map<std::string, std::string> model;
    Random rnd(20260727);

    for (int op = 1; op <= 30000; ++op) {
        const std::string key = key_for(rnd.uniform(kKeySpace));
        if (rnd.uniform(100) < 70) {
            const std::string value =
                "v" + std::to_string(op) + "-" + std::string(rnd.uniform(180), 'x');
            ASSERT_TRUE(db_->put(WriteOptions(), key, value).ok());
            model[key] = value;
        } else {
            ASSERT_TRUE(db_->remove(WriteOptions(), key).ok());
            model.erase(key);
        }

        if (op % 1500 == 0) {
            verify_point_gets(model, rnd);
            verify_against(model, nullptr, "live view @op " + std::to_string(op));
            verify_snapshots();
        }
        if (op % 4000 == 0 && snapshots_.size() < 3) {
            snapshots_.emplace_back(db_->get_snapshot(), model);
        }
        if (op % 10000 == 0) {
            reopen(); // recovery must reconstruct the exact model state
            verify_against(model, nullptr, "post-reopen @op " + std::to_string(op));
        }
    }

    verify_snapshots();
    ASSERT_TRUE(db_->compact_all().ok());
    verify_against(model, nullptr, "post-compact_all");
    verify_snapshots();

    reopen();
    verify_against(model, nullptr, "final reopen");

    EXPECT_GT(total_flushes_, 10u) << "tiny buffers should have flushed constantly";
    EXPECT_GT(total_compactions_, 5u);
}

// Batches must be atomic across flush/compaction/reopen boundaries.
TEST_F(ModelTest, BatchesAreAtomicUnderChurn) {
    std::map<std::string, std::string> model;
    Random rnd(7);
    for (int round = 0; round < 400; ++round) {
        WriteBatch batch;
        const int n = 1 + static_cast<int>(rnd.uniform(20));
        for (int i = 0; i < n; ++i) {
            const std::string key = key_for(rnd.uniform(kKeySpace));
            if (rnd.uniform(100) < 75) {
                const std::string value = "r" + std::to_string(round) + "i" + std::to_string(i);
                batch.put(key, value);
                model[key] = value;
            } else {
                batch.remove(key);
                model.erase(key);
            }
        }
        ASSERT_TRUE(db_->write(WriteOptions(), &batch).ok());
        if (round % 100 == 0) {
            verify_against(model, nullptr, "batch churn round " + std::to_string(round));
            reopen();
        }
    }
    verify_against(model, nullptr, "batch churn final");
}

} // namespace
} // namespace strata
