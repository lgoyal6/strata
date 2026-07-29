#pragma once

#include "db/dbformat.h"
#include "strata/db.h"

namespace strata {

class SnapshotImpl final : public Snapshot {
  public:
    SequenceNumber sequence = 0;
    SnapshotImpl* prev = nullptr;
    SnapshotImpl* next = nullptr;
};

// Circular doubly-linked list ordered oldest -> newest. Caller locks (DB
// mutex); compaction reads oldest() to bound what it may garbage-collect.
class SnapshotList {
  public:
    SnapshotList() {
        head_.prev = &head_;
        head_.next = &head_;
    }

    bool empty() const {
        return head_.next == &head_;
    }

    const SnapshotImpl* oldest() const {
        return head_.next;
    }

    SnapshotImpl* new_snapshot(SequenceNumber seq) {
        auto* s = new SnapshotImpl;
        s->sequence = seq;
        s->next = &head_;
        s->prev = head_.prev;
        s->prev->next = s;
        s->next->prev = s;
        return s;
    }

    void release(const SnapshotImpl* s) {
        s->prev->next = s->next;
        s->next->prev = s->prev;
        delete s;
    }

  private:
    SnapshotImpl head_;
};

} // namespace strata
