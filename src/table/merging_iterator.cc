#include "table/merging_iterator.h"

#include <cassert>

#include "util/scan_probe.h"

namespace strata {
namespace {

class MergingIterator final : public Iterator {
  public:
    MergingIterator(const InternalKeyComparator* cmp,
                    std::vector<std::unique_ptr<Iterator>> children)
        : cmp_(cmp), children_(std::move(children)) {
        STRATA_PROBE_ADD(children, children_.size());
    }

    bool valid() const override {
        return current_ != nullptr;
    }

    void seek_to_first() override {
        for (auto& child : children_) {
            child->seek_to_first();
        }
        find_smallest();
    }

    void seek(const Slice& target) override {
        for (auto& child : children_) {
            child->seek(target);
        }
        find_smallest();
    }

    void next() override {
        assert(valid());
        current_->next();
        STRATA_PROBE_ADD(internal_next, 1);
        find_smallest();
    }

    Slice key() const override {
        assert(valid());
        return current_->key();
    }

    Slice value() const override {
        assert(valid());
        return current_->value();
    }

    Status status() const override {
        for (const auto& child : children_) {
            const Status s = child->status();
            if (!s.ok()) {
                return s;
            }
        }
        return Status::okay();
    }

  private:
    // Linear scan: child count is small (memtables + L0 files + one
    // concatenating iterator per level), so a heap buys nothing here.
    void find_smallest() {
        Iterator* smallest = nullptr;
        STRATA_PROBE_ADD(compares, children_.size());
        for (auto& child : children_) {
            if (child->valid() &&
                (smallest == nullptr || cmp_->compare(child->key(), smallest->key()) < 0)) {
                smallest = child.get();
            }
        }
        current_ = smallest;
    }

    const InternalKeyComparator* cmp_;
    std::vector<std::unique_ptr<Iterator>> children_;
    Iterator* current_ = nullptr;
};

} // namespace

Iterator* new_merging_iterator(const InternalKeyComparator* cmp,
                               std::vector<std::unique_ptr<Iterator>> children) {
    return new MergingIterator(cmp, std::move(children));
}

} // namespace strata
