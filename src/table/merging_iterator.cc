#include "table/merging_iterator.h"

#include <algorithm>
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
#ifdef STRATA_MERGE_HEAP
        advance_top(); // O(log k): only the child that moved is re-sifted
#else
        find_smallest();
#endif
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
    // concatenating iterator per level), so a heap buys nothing here. The
    // measured comparison behind that claim, including the fan-in at which a
    // heap would start to pay, is in docs/BENCHMARKS.md; build with
    // -DSTRATA_MERGE_HEAP=ON to run the other arm.
#ifndef STRATA_MERGE_HEAP
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
#else
    // Heap arm: the heap is built once when the iterator is positioned, and each
    // advance re-sifts only the child that moved (pop_heap + push_heap), so a
    // step costs O(log k) rather than the linear arm's O(k) sweep. Building it
    // fresh on every advance would be a strawman: that is O(k) plus heap
    // overhead, and could only ever lose.
    struct Worse {
        const InternalKeyComparator* cmp;
        bool operator()(Iterator* a, Iterator* b) const {
            return cmp->compare(a->key(), b->key()) > 0; // min-heap: smallest on top
        }
    };

    void find_smallest() { // full rebuild, used when the iterator is repositioned
        STRATA_PROBE_ADD(compares, children_.size());
        heap_.clear();
        for (auto& child : children_) {
            if (child->valid()) {
                heap_.push_back(child.get());
            }
        }
        std::make_heap(heap_.begin(), heap_.end(), Worse{cmp_});
        current_ = heap_.empty() ? nullptr : heap_.front();
    }

    void advance_top() { // the caller has already advanced heap_.front()
        STRATA_PROBE_ADD(compares, 2);
        std::pop_heap(heap_.begin(), heap_.end(), Worse{cmp_});
        if (heap_.back()->valid()) {
            std::push_heap(heap_.begin(), heap_.end(), Worse{cmp_});
        } else {
            heap_.pop_back();
        }
        current_ = heap_.empty() ? nullptr : heap_.front();
    }

    std::vector<Iterator*> heap_;
#endif

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
