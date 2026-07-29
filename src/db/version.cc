#include "db/version.h"

#include <algorithm>
#include <cassert>
#include <random>

#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {

// ===========================================================================
// MANIFEST serialization
// ===========================================================================

namespace {
constexpr std::uint32_t kManifestVersion = 1;
} // namespace

void encode_manifest(const ManifestData& in, std::string* out) {
    std::string payload;
    put_fixed32(&payload, kManifestVersion);
    put_fixed64(&payload, in.db_uuid);
    put_fixed64(&payload, in.next_file_number);
    put_fixed64(&payload, in.last_sequence);
    put_fixed64(&payload, in.min_wal_number);
    for (int level = 0; level < kNumLevels; ++level) {
        put_fixed32(&payload, static_cast<std::uint32_t>(in.files[level].size()));
        for (const auto& f : in.files[level]) {
            put_fixed64(&payload, f.number);
            put_fixed64(&payload, f.file_size);
            put_length_prefixed_slice(&payload, Slice(f.smallest));
            put_length_prefixed_slice(&payload, Slice(f.largest));
        }
    }
    out->clear();
    put_fixed32(out, crc32c_mask(crc32c(payload.data(), payload.size())));
    put_fixed32(out, static_cast<std::uint32_t>(payload.size()));
    out->append(payload);
}

Status parse_manifest(const Slice& contents, ManifestData* out) {
    if (contents.size() < 8) {
        return Status::corruption("manifest too small");
    }
    const std::uint32_t stored_crc = crc32c_unmask(decode_fixed32(contents.data()));
    const std::uint32_t length = decode_fixed32(contents.data() + 4);
    if (length != contents.size() - 8) {
        return Status::corruption("manifest length mismatch");
    }
    Slice payload(contents.data() + 8, length);
    if (crc32c(payload.data(), payload.size()) != stored_crc) {
        return Status::corruption("manifest checksum mismatch");
    }
    if (payload.size() < 4 + 8 * 4) {
        return Status::corruption("manifest payload too small");
    }
    if (decode_fixed32(payload.data()) != kManifestVersion) {
        return Status::corruption("unsupported manifest version");
    }
    payload.remove_prefix(4);
    out->db_uuid = decode_fixed64(payload.data());
    out->next_file_number = decode_fixed64(payload.data() + 8);
    out->last_sequence = decode_fixed64(payload.data() + 16);
    out->min_wal_number = decode_fixed64(payload.data() + 24);
    payload.remove_prefix(32);

    for (int level = 0; level < kNumLevels; ++level) {
        if (payload.size() < 4) {
            return Status::corruption("manifest truncated at level header");
        }
        const std::uint32_t count = decode_fixed32(payload.data());
        payload.remove_prefix(4);
        out->files[level].clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            if (payload.size() < 16) {
                return Status::corruption("manifest truncated at file entry");
            }
            ManifestData::File f;
            f.number = decode_fixed64(payload.data());
            f.file_size = decode_fixed64(payload.data() + 8);
            payload.remove_prefix(16);
            Slice smallest, largest;
            if (!get_length_prefixed_slice(&payload, &smallest) ||
                !get_length_prefixed_slice(&payload, &largest) || smallest.size() < 8 ||
                largest.size() < 8) {
                return Status::corruption("manifest bad file keys");
            }
            f.smallest = smallest.to_string();
            f.largest = largest.to_string();
            out->files[level].push_back(std::move(f));
        }
    }
    if (!payload.empty()) {
        return Status::corruption("manifest trailing bytes");
    }
    return Status::okay();
}

// ===========================================================================
// Version
// ===========================================================================

std::uint64_t Version::level_bytes(int level) const {
    std::uint64_t total = 0;
    for (const auto& f : files[level]) {
        total += f->file_size;
    }
    return total;
}

void Version::overlapping_inputs(int level, const Slice& begin_ukey, const Slice& end_ukey,
                                 std::vector<std::shared_ptr<FileMeta>>* inputs) const {
    inputs->clear();
    for (const auto& f : files[level]) {
        if (f->largest.user_key().compare(begin_ukey) < 0 ||
            f->smallest.user_key().compare(end_ukey) > 0) {
            continue;
        }
        inputs->push_back(f);
    }
}

namespace {

// Probes one table file for the lookup key. Returns true when the search is
// decided (found / tombstone / error) — false means "keep looking deeper".
bool probe_file(TableCache* tc, const InternalKeyComparator* /*icmp*/, const FileMeta& f,
                const LookupKey& lkey, std::string* value, Status* result) {
    std::shared_ptr<TableReader> reader;
    Status s = tc->find_table(f.number, f.file_size, &reader);
    if (!s.ok()) {
        *result = s;
        return true;
    }
    std::string found_ikey, found_value;
    bool found = false;
    s = reader->get(lkey.internal_key(), &found_ikey, &found_value, &found);
    if (!s.ok()) {
        *result = s;
        return true;
    }
    if (!found) {
        return false;
    }
    const std::uint64_t tag = extract_tag(Slice(found_ikey));
    if (static_cast<ValueType>(tag & 0xffu) == kTypeDeletion) {
        *result = Status::not_found();
        return true;
    }
    value->assign(found_value);
    *result = Status::okay();
    return true;
}

} // namespace

Status Version::get(const LookupKey& lkey, std::string* value) {
    TableCache* tc = vset_->table_cache();
    const InternalKeyComparator* icmp = vset_->icmp();
    const Slice ukey = lkey.user_key();
    const Slice ikey = lkey.internal_key();
    Status result;

    // L0: files may overlap; newest file number first. Flush order makes
    // file-number order equal to sequence order for L0.
    for (auto it = files[0].rbegin(); it != files[0].rend(); ++it) {
        const FileMeta& f = **it;
        if (ukey.compare(f.smallest.user_key()) < 0 || ukey.compare(f.largest.user_key()) > 0) {
            continue;
        }
        if (probe_file(tc, icmp, f, lkey, value, &result)) {
            return result;
        }
    }

    // Deeper levels: at most one candidate file per level.
    for (int level = 1; level < kNumLevels; ++level) {
        const auto& lf = files[level];
        if (lf.empty()) {
            continue;
        }
        // First file whose largest >= ikey.
        const auto pos = std::lower_bound(
            lf.begin(), lf.end(), ikey, [icmp](const std::shared_ptr<FileMeta>& f, const Slice& k) {
                return icmp->compare(f->largest.encoded(), k) < 0;
            });
        if (pos == lf.end()) {
            continue;
        }
        const FileMeta& f = **pos;
        if (ukey.compare(f.smallest.user_key()) < 0) {
            continue;
        }
        if (probe_file(tc, icmp, f, lkey, value, &result)) {
            return result;
        }
    }
    return Status::not_found();
}

// Concatenating iterator over one level's disjoint, sorted files.
namespace {

class LevelIterator final : public Iterator {
  public:
    LevelIterator(TableCache* tc, const InternalKeyComparator* icmp,
                  std::vector<std::shared_ptr<FileMeta>> flist)
        : tc_(tc), icmp_(icmp), files_(std::move(flist)) {}

    bool valid() const override {
        return data_iter_ != nullptr && data_iter_->valid();
    }

    void seek_to_first() override {
        index_ = 0;
        init_file_iterator();
        if (data_iter_ != nullptr) {
            data_iter_->seek_to_first();
        }
        skip_empty_files();
    }

    void seek(const Slice& target) override {
        // First file whose largest >= target.
        const auto pos =
            std::lower_bound(files_.begin(), files_.end(), target,
                             [this](const std::shared_ptr<FileMeta>& f, const Slice& k) {
                                 return icmp_->compare(f->largest.encoded(), k) < 0;
                             });
        index_ = static_cast<std::size_t>(pos - files_.begin());
        init_file_iterator();
        if (data_iter_ != nullptr) {
            data_iter_->seek(target);
        }
        skip_empty_files();
    }

    void next() override {
        assert(valid());
        data_iter_->next();
        skip_empty_files();
    }

    Slice key() const override {
        return data_iter_->key();
    }
    Slice value() const override {
        return data_iter_->value();
    }

    Status status() const override {
        if (!status_.ok()) {
            return status_;
        }
        if (data_iter_ != nullptr) {
            return data_iter_->status();
        }
        return Status::okay();
    }

  private:
    void init_file_iterator() {
        data_iter_.reset();
        if (index_ >= files_.size()) {
            return;
        }
        std::shared_ptr<TableReader> reader;
        const Status s =
            tc_->find_table(files_[index_]->number, files_[index_]->file_size, &reader);
        if (!s.ok()) {
            status_ = s;
            return;
        }
        data_iter_.reset(reader->new_iterator());
    }

    void skip_empty_files() {
        while (status_.ok() && (data_iter_ == nullptr || !data_iter_->valid())) {
            if (data_iter_ != nullptr && !data_iter_->status().ok()) {
                status_ = data_iter_->status();
                return;
            }
            if (index_ + 1 >= files_.size()) {
                data_iter_.reset();
                return;
            }
            ++index_;
            init_file_iterator();
            if (data_iter_ != nullptr) {
                data_iter_->seek_to_first();
            }
        }
    }

    TableCache* const tc_;
    const InternalKeyComparator* const icmp_;
    const std::vector<std::shared_ptr<FileMeta>> files_;
    std::size_t index_ = 0;
    std::unique_ptr<Iterator> data_iter_;
    Status status_;
};

} // namespace

void Version::add_iterators(std::vector<std::unique_ptr<Iterator>>* iters) {
    TableCache* tc = vset_->table_cache();
    // L0 files newest-first so merging-iterator ties (duplicate internal
    // keys from a crash during recovery) resolve to the newer copy.
    for (auto it = files[0].rbegin(); it != files[0].rend(); ++it) {
        std::shared_ptr<TableReader> reader;
        const Status s = tc->find_table((*it)->number, (*it)->file_size, &reader);
        if (s.ok()) {
            iters->emplace_back(reader->new_iterator());
        }
        // An unopenable live file will surface as an error on read paths;
        // iterators fail via status when actually touched.
    }
    for (int level = 1; level < kNumLevels; ++level) {
        if (!files[level].empty()) {
            iters->emplace_back(std::make_unique<LevelIterator>(tc, vset_->icmp(), files[level]));
        }
    }
}

// ===========================================================================
// VersionSet
// ===========================================================================

VersionSet::VersionSet(Env* env, std::string dbname, const Options* options,
                       TableCache* table_cache, const InternalKeyComparator* icmp)
    : env_(env), dbname_(std::move(dbname)), options_(options), table_cache_(table_cache),
      icmp_(icmp) {}

std::shared_ptr<Version> VersionSet::current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
}

void VersionSet::install(std::shared_ptr<Version> v) {
    std::lock_guard<std::mutex> lock(mu_);
    live_versions_.push_back(v);
    current_ = std::move(v);
}

void VersionSet::bump_file_number_floor(std::uint64_t floor) {
    std::uint64_t cur = next_file_number_.load(std::memory_order_relaxed);
    while (cur < floor && !next_file_number_.compare_exchange_weak(cur, floor)) {
    }
}

void VersionSet::add_live_files(std::set<std::uint64_t>* live) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = live_versions_.begin(); it != live_versions_.end();) {
        if (const auto v = it->lock()) {
            for (int level = 0; level < kNumLevels; ++level) {
                for (const auto& f : v->files[level]) {
                    live->insert(f->number);
                }
            }
            ++it;
        } else {
            it = live_versions_.erase(it);
        }
    }
}

Status VersionSet::recover(bool* created_new) {
    *created_new = false;
    const std::string manifest_path = manifest_file_name(dbname_);

    if (!env_->file_exists(manifest_path)) {
        if (!options_->create_if_missing) {
            return Status::invalid_argument(dbname_ + ": no MANIFEST (create_if_missing=false)");
        }
        std::random_device rd;
        db_uuid_ = (static_cast<std::uint64_t>(rd()) << 32) | rd();
        if (db_uuid_ == 0) {
            db_uuid_ = 1;
        }
        auto v = std::make_shared<Version>(this);
        install(std::move(v));
        const Status s = write_manifest(*current());
        if (!s.ok()) {
            return s;
        }
        *created_new = true;
        return Status::okay();
    }

    if (options_->error_if_exists) {
        return Status::invalid_argument(dbname_ + ": exists (error_if_exists=true)");
    }

    std::uint64_t size = 0;
    Status s = env_->get_file_size(manifest_path, &size);
    if (!s.ok()) {
        return s;
    }
    std::unique_ptr<SequentialFile> file;
    s = env_->new_sequential_file(manifest_path, &file);
    if (!s.ok()) {
        return s;
    }
    std::string contents;
    contents.resize(size);
    std::size_t got = 0;
    while (got < size) {
        Slice chunk;
        s = file->read(size - got, &chunk, contents.data() + got);
        if (!s.ok()) {
            return s;
        }
        if (chunk.empty()) {
            return Status::io_error(manifest_path + ": short read");
        }
        got += chunk.size();
    }

    ManifestData data;
    s = parse_manifest(Slice(contents), &data);
    if (!s.ok()) {
        return s;
    }

    auto v = std::make_shared<Version>(this);
    for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& mf : data.files[level]) {
            auto f = std::make_shared<FileMeta>();
            f->number = mf.number;
            f->file_size = mf.file_size;
            f->smallest.decode_from(Slice(mf.smallest));
            f->largest.decode_from(Slice(mf.largest));
            v->files[level].push_back(std::move(f));
        }
    }
    db_uuid_ = data.db_uuid;
    next_file_number_.store(data.next_file_number, std::memory_order_relaxed);
    last_sequence_.store(data.last_sequence, std::memory_order_relaxed);
    min_wal_number_ = data.min_wal_number;
    install(std::move(v));
    return Status::okay();
}

Status VersionSet::write_manifest(const Version& v) {
    ManifestData data;
    data.db_uuid = db_uuid_;
    data.next_file_number = next_file_number_.load(std::memory_order_relaxed);
    data.last_sequence = last_sequence_.load(std::memory_order_relaxed);
    data.min_wal_number = min_wal_number_;
    for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& f : v.files[level]) {
            ManifestData::File mf;
            mf.number = f->number;
            mf.file_size = f->file_size;
            mf.smallest = f->smallest.encoded().to_string();
            mf.largest = f->largest.encoded().to_string();
            data.files[level].push_back(std::move(mf));
        }
    }
    std::string contents;
    encode_manifest(data, &contents);

    // tmp + fsync + atomic rename + dir fsync (docs/DESIGN.md §1.3). The
    // MANIFEST is always synced regardless of the WAL fsync policy.
    const std::string tmp = temp_manifest_file_name(dbname_);
    std::unique_ptr<WritableFile> file;
    Status s = env_->new_writable_file(tmp, &file);
    if (!s.ok()) {
        return s;
    }
    s = file->append(Slice(contents));
    if (s.ok()) {
        s = file->sync(options_->use_fullfsync);
    }
    if (s.ok()) {
        s = file->close();
    }
    if (s.ok()) {
        s = env_->rename_file(tmp, manifest_file_name(dbname_));
    }
    if (s.ok()) {
        s = env_->sync_dir(dbname_);
    }
    return s;
}

Status VersionSet::log_and_apply(VersionEdit* edit) {
    const auto base = current();
    auto v = std::make_shared<Version>(this);

    for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& f : base->files[level]) {
            bool deleted = false;
            for (const auto& [dl, dn] : edit->deleted_files) {
                if (dl == level && dn == f->number) {
                    deleted = true;
                    break;
                }
            }
            if (!deleted) {
                v->files[level].push_back(f);
            }
        }
    }
    for (const auto& [level, f] : edit->added_files) {
        v->files[level].push_back(f);
    }
    // L0 by file number (== flush order == sequence order); deeper levels by
    // smallest key.
    std::sort(v->files[0].begin(), v->files[0].end(),
              [](const auto& a, const auto& b) { return a->number < b->number; });
    for (int level = 1; level < kNumLevels; ++level) {
        std::sort(v->files[level].begin(), v->files[level].end(),
                  [this](const auto& a, const auto& b) {
                      return icmp_->compare(a->smallest.encoded(), b->smallest.encoded()) < 0;
                  });
    }
    if (edit->min_wal_number.has_value()) {
        assert(*edit->min_wal_number >= min_wal_number_);
        min_wal_number_ = *edit->min_wal_number;
    }

    const Status s = write_manifest(*v);
    if (!s.ok()) {
        return s; // current_ unchanged: the failed version never existed
    }
    install(std::move(v));
    return Status::okay();
}

std::uint64_t VersionSet::target_bytes(int level) const {
    assert(level >= 1);
    double target = static_cast<double>(options_->l1_target_bytes);
    for (int i = 1; i < level; ++i) {
        target *= options_->level_size_multiplier;
    }
    return static_cast<std::uint64_t>(target);
}

bool VersionSet::pick_compaction(CompactionJob* job) {
    const auto v = current();
    int best_level = -1;
    double best_score = 1.0; // strictly above 1.0 triggers

    const double l0_score = static_cast<double>(v->files[0].size()) /
                            static_cast<double>(options_->l0_compaction_trigger);
    if (l0_score >= best_score) {
        best_score = l0_score;
        best_level = 0;
    }
    for (int level = 1; level < kNumLevels - 1; ++level) {
        if (v->files[level].empty()) {
            continue;
        }
        const double score =
            static_cast<double>(v->level_bytes(level)) / static_cast<double>(target_bytes(level));
        if (score > best_score) {
            best_score = score;
            best_level = level;
        }
    }
    if (best_level < 0) {
        return false;
    }
    job->level = best_level;
    fill_inputs(job, v);
    return true;
}

bool VersionSet::pick_compaction_at_level(int level, CompactionJob* job) {
    const auto v = current();
    if (level < 0 || level >= kNumLevels - 1 || v->files[level].empty()) {
        return false;
    }
    job->level = level;
    fill_inputs(job, v);
    return true;
}

namespace {

// The LevelDB boundary bug: two files in the same level may share a boundary
// user key (same key, different sequences). Compacting one without the other
// would let a stale version surface. Expand inputs until no file outside the
// set starts with the user key the set ends with.
void add_boundary_inputs(const InternalKeyComparator* icmp,
                         const std::vector<std::shared_ptr<FileMeta>>& level_files,
                         std::vector<std::shared_ptr<FileMeta>>* inputs) {
    bool grew = true;
    while (grew) {
        grew = false;
        // Largest key currently in the input set.
        const FileMeta* largest_file = nullptr;
        for (const auto& f : *inputs) {
            if (largest_file == nullptr ||
                icmp->compare(f->largest.encoded(), largest_file->largest.encoded()) > 0) {
                largest_file = f.get();
            }
        }
        if (largest_file == nullptr) {
            return;
        }
        for (const auto& f : level_files) {
            if (icmp->compare(f->smallest.encoded(), largest_file->largest.encoded()) > 0 &&
                f->smallest.user_key() == largest_file->largest.user_key()) {
                bool already = false;
                for (const auto& in : *inputs) {
                    if (in->number == f->number) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    inputs->push_back(f);
                    grew = true;
                }
            }
        }
    }
}

void user_key_range(const std::vector<std::shared_ptr<FileMeta>>& files, std::string* begin,
                    std::string* end) {
    for (const auto& f : files) {
        const Slice s = f->smallest.user_key();
        const Slice l = f->largest.user_key();
        if (begin->empty() || s.compare(Slice(*begin)) < 0) {
            begin->assign(s.data(), s.size());
        }
        if (end->empty() || l.compare(Slice(*end)) > 0) {
            end->assign(l.data(), l.size());
        }
    }
}

} // namespace

void VersionSet::fill_inputs(CompactionJob* job, std::shared_ptr<Version> v) {
    const int level = job->level;
    job->inputs[0].clear();
    job->inputs[1].clear();
    job->trivial_move = false;

    if (level == 0) {
        // Size-tiered step: take all of L0 (files overlap each other).
        job->inputs[0] = v->files[0];
    } else {
        // Round-robin cursor: first file starting after the last compacted key.
        const auto& lf = v->files[level];
        std::size_t pick = 0;
        if (!compact_cursor_[level].empty()) {
            for (std::size_t i = 0; i < lf.size(); ++i) {
                if (lf[i]->largest.user_key().compare(Slice(compact_cursor_[level])) > 0) {
                    pick = i;
                    break;
                }
            }
        }
        job->inputs[0].push_back(lf[pick]);
        add_boundary_inputs(icmp_, lf, &job->inputs[0]);
    }

    std::string begin, end;
    user_key_range(job->inputs[0], &begin, &end);
    v->overlapping_inputs(level + 1, Slice(begin), Slice(end), &job->inputs[1]);
    // Files in level+1 can share boundary keys too.
    if (!job->inputs[1].empty()) {
        add_boundary_inputs(icmp_, v->files[level + 1], &job->inputs[1]);
    }

    if (level > 0 && job->inputs[0].size() == 1 && job->inputs[1].empty()) {
        job->trivial_move = true;
    }
    job->base = std::move(v);

    // Advance the cursor past this compaction's range.
    compact_cursor_[level] = end;
}

} // namespace strata
