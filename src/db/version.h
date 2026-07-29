#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/table_cache.h"
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"
#include "util/env.h"

namespace strata {

struct FileMeta {
    std::uint64_t number = 0;
    std::uint64_t file_size = 0;
    InternalKey smallest;
    InternalKey largest;
};

class VersionSet;

// Immutable snapshot of the file DAG. Readers/iterators hold a shared_ptr,
// which is what keeps compaction from deleting files under them.
class Version {
  public:
    explicit Version(VersionSet* vset) : vset_(vset) {}
    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    // L0 sorted by file number ascending (probe newest-first by walking
    // backwards); L1+ sorted by smallest key, user-key-disjoint.
    std::vector<std::shared_ptr<FileMeta>> files[kNumLevels];

    // Point lookup through the levels (memtables are DBImpl's job).
    Status get(const LookupKey& lkey, std::string* value);

    // Appends iterators covering every file: one per L0 file, one
    // concatenating iterator per deeper level.
    void add_iterators(std::vector<std::unique_ptr<Iterator>>* iters);

    std::uint64_t level_bytes(int level) const;

    // Files in `level` whose user-key range intersects [begin, end].
    void overlapping_inputs(int level, const Slice& begin_ukey, const Slice& end_ukey,
                            std::vector<std::shared_ptr<FileMeta>>* inputs) const;

  private:
    friend class VersionSet;
    VersionSet* const vset_;
};

// Delta applied by flush/compaction; log_and_apply turns it into a new
// durable Version.
struct VersionEdit {
    std::vector<std::pair<int, std::shared_ptr<FileMeta>>> added_files;
    std::vector<std::pair<int, std::uint64_t>> deleted_files;
    std::optional<std::uint64_t> min_wal_number;
};

struct CompactionJob {
    int level = -1; // merging `level` into `level+1`
    std::vector<std::shared_ptr<FileMeta>> inputs[2];
    std::shared_ptr<Version> base; // pins input files while running
    bool trivial_move = false;     // single input, no overlap: rename only
};

// --- MANIFEST serialization (free functions so the fuzzer can reach the
// parser directly; docs/DESIGN.md §1.3) ---------------------------------

struct ManifestData {
    std::uint64_t db_uuid = 0;
    std::uint64_t next_file_number = 1;
    SequenceNumber last_sequence = 0;
    std::uint64_t min_wal_number = 0;
    struct File {
        std::uint64_t number = 0;
        std::uint64_t file_size = 0;
        std::string smallest;
        std::string largest;
    };
    std::vector<File> files[kNumLevels];
};

// Produces the full file contents (CRC header included).
void encode_manifest(const ManifestData& in, std::string* out);
// Parses full file contents; every byte treated as adversarial.
Status parse_manifest(const Slice& contents, ManifestData* out);

// -----------------------------------------------------------------------

class VersionSet {
  public:
    VersionSet(Env* env, std::string dbname, const Options* options, TableCache* table_cache,
               const InternalKeyComparator* icmp);

    // Loads the MANIFEST, or creates a fresh durable one for a new DB.
    Status recover(bool* created_new);

    // Applies edit to current -> new Version, writes the MANIFEST durably
    // (tmp + fsync + rename + dir fsync), installs the new Version.
    // Caller must serialize calls (DB mutex).
    Status log_and_apply(VersionEdit* edit);

    std::shared_ptr<Version> current() const;

    std::uint64_t new_file_number() {
        return next_file_number_.fetch_add(1, std::memory_order_relaxed);
    }
    // Recovery: ensure the counter clears every file seen on disk.
    void bump_file_number_floor(std::uint64_t floor);

    SequenceNumber last_sequence() const {
        return last_sequence_.load(std::memory_order_acquire);
    }
    void set_last_sequence(SequenceNumber s) {
        last_sequence_.store(s, std::memory_order_release);
    }

    std::uint64_t min_wal_number() const {
        return min_wal_number_;
    }
    std::uint64_t db_uuid() const {
        return db_uuid_;
    }

    // Union of file numbers referenced by any still-referenced Version.
    void add_live_files(std::set<std::uint64_t>* live);

    // Picks the highest-score compaction; false if all scores < 1.
    bool pick_compaction(CompactionJob* job);
    // Forces a compaction of `level` regardless of score (compact_all).
    bool pick_compaction_at_level(int level, CompactionJob* job);

    TableCache* table_cache() {
        return table_cache_;
    }
    const InternalKeyComparator* icmp() const {
        return icmp_;
    }
    const Options* options() const {
        return options_;
    }

  private:
    friend class Version;

    std::uint64_t target_bytes(int level) const;
    void fill_inputs(CompactionJob* job, std::shared_ptr<Version> v);
    Status write_manifest(const Version& v);
    void install(std::shared_ptr<Version> v);

    Env* const env_;
    const std::string dbname_;
    const Options* const options_;
    TableCache* const table_cache_;
    const InternalKeyComparator* const icmp_;

    mutable std::mutex mu_; // guards current_ swap + live_versions_
    std::shared_ptr<Version> current_;
    std::list<std::weak_ptr<Version>> live_versions_;

    std::atomic<std::uint64_t> next_file_number_{1};
    std::atomic<SequenceNumber> last_sequence_{0};
    std::uint64_t min_wal_number_ = 0;
    std::uint64_t db_uuid_ = 0;
    std::string compact_cursor_[kNumLevels]; // round-robin user-key cursor
};

} // namespace strata
