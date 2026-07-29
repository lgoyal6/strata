#include "db/db_iter.h"

#include <cassert>

namespace strata {
namespace {

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
                internal_->next();
                continue;
            }
            if (skipping && pik.user_key.compare(Slice(saved_key_)) <= 0) {
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
                valid_ = true;
                return;
            }
        }
        valid_ = false;
    }

    std::unique_ptr<Iterator> internal_;
    const SequenceNumber seq_;
    std::shared_ptr<void> pin_;
    std::string saved_key_; // owned copy: block iterators mutate on next()
    bool valid_ = false;
    Status status_;
};

} // namespace

Iterator* new_db_iterator(std::unique_ptr<Iterator> internal, SequenceNumber snapshot_seq,
                          std::shared_ptr<void> pin) {
    return new DBIter(std::move(internal), snapshot_seq, std::move(pin));
}

} // namespace strata
