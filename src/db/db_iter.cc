#include "db/db_iter.h"

#include "util/scan_probe.h"

#include <cassert>

namespace strata {
namespace {

// A key that has been overwritten many times leaves a run of superseded
// versions that the iterator must step over. Walking them one at a time
// costs one N-way merge comparison each; past this run length a single
// targeted seek to the next user key is cheaper. Measured sweep and the
// workload that motivates it: docs/BENCHMARKS.md.
#ifndef STRATA_SKIP_RUN_SEEK_THRESHOLD
#define STRATA_SKIP_RUN_SEEK_THRESHOLD 16
#endif
constexpr std::uint64_t kSkipRunSeekThreshold = STRATA_SKIP_RUN_SEEK_THRESHOLD;

class DBIter final : public Iterator {
  public:
    DBIter(std::unique_ptr<Iterator> internal, SequenceNumber seq, std::shared_ptr<void> pin)
        : internal_(std::move(internal)), seq_(seq), pin_(std::move(pin)) {}

    bool valid() const override {
        return valid_;
    }

    void seek_to_first() override {
        internal_->seek_to_first();
        find_next_user_entry(false);
    }

    void seek(const Slice& target) override {
        std::string ikey;
        append_internal_key(&ikey, target, seq_, kValueTypeForSeek);
        internal_->seek(Slice(ikey));
        find_next_user_entry(false);
    }

    void next() override {
        assert(valid_);
        // saved_key_ holds the key just yielded; skip its remaining versions.
        internal_->next();
        find_next_user_entry(true);
    }

    Slice key() const override {
        assert(valid_);
        return Slice(saved_key_);
    }

    Slice value() const override {
        assert(valid_);
        // internal_ is parked on the accepted entry, so its value is live.
        return internal_->value();
    }

    Status status() const override {
        if (!status_.ok()) {
            return status_;
        }
        return internal_->status();
    }

  private:
    // Advances internal_ to the next entry this snapshot can see. When
    // `skipping`, entries with user key <= saved_key_ are shadowed (older
    // versions of a yielded key, or anything under a tombstone).
    void find_next_user_entry(bool skipping) {
        std::uint64_t run = 0; // consecutive superseded versions of saved_key_
        while (internal_->valid()) {
            ParsedInternalKey pik;
            if (!parse_internal_key(internal_->key(), &pik)) {
                status_ = Status::corruption("malformed internal key in iterator");
                valid_ = false;
                return;
            }
            // Visibility BEFORE type: an invisible tombstone must not hide a
            // visible older Put.
            if (pik.sequence > seq_) {
                STRATA_PROBE_ADD(skipped, 1);
                STRATA_PROBE_ADD(skipped_seq, 1);
                internal_->next();
                continue;
            }
            if (skipping && pik.user_key.compare(Slice(saved_key_)) <= 0) {
                STRATA_PROBE_ADD(skipped, 1);
                STRATA_PROBE_ADD(skipped_key, 1);
#ifdef STRATA_SCAN_PROBE
                if (++run_ > ::strata::probe::counters().max_run.load(std::memory_order_relaxed)) {
                    ::strata::probe::counters().max_run.store(run_, std::memory_order_relaxed);
                }
#endif
                if (++run >= kSkipRunSeekThreshold) {
                    seek_past_saved_key();
                    run = 0;
                    continue;
                }
                internal_->next();
                continue;
            }
            switch (pik.type) {
            case kTypeDeletion:
                // Every older version of this key is shadowed.
                saved_key_.assign(pik.user_key.data(), pik.user_key.size());
                skipping = true;
                internal_->next();
                break;
            case kTypeValue:
                saved_key_.assign(pik.user_key.data(), pik.user_key.size());
#ifdef STRATA_SCAN_PROBE
                run_ = 0;
#endif
                valid_ = true;
                return;
            }
        }
        valid_ = false;
    }

    // Positions internal_ at the first entry whose user key is strictly
    // greater than saved_key_. User keys compare bytewise, so appending a
    // zero byte yields the smallest user key above saved_key_; seeking at
    // kMaxSequenceNumber lands on that key's newest version, and the normal
    // visibility check in the caller still applies.
    void seek_past_saved_key() {
        std::string next_user_key = saved_key_;
        next_user_key.push_back('\0');
        std::string target;
        append_internal_key(&target, Slice(next_user_key), kMaxSequenceNumber, kValueTypeForSeek);
        internal_->seek(Slice(target));
        STRATA_PROBE_ADD(skip_seeks, 1);
    }

    std::unique_ptr<Iterator> internal_;
    const SequenceNumber seq_;
    std::shared_ptr<void> pin_;
    std::string saved_key_; // owned copy: block iterators mutate on next()
#ifdef STRATA_SCAN_PROBE
    std::uint64_t run_ = 0;
#endif
    bool valid_ = false;
    Status status_;
};

} // namespace

Iterator* new_db_iterator(std::unique_ptr<Iterator> internal, SequenceNumber snapshot_seq,
                          std::shared_ptr<void> pin) {
    return new DBIter(std::move(internal), snapshot_seq, std::move(pin));
}

} // namespace strata
