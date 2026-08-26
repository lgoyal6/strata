#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "strata/iterator.h"
#include "strata/status.h"
#include "util/arena.h"

namespace strata {

// Arena-backed sorted run of internal-key entries. Held by shared_ptr:
// the DB (as active or immutable), the flush job, and every in-flight
// read/iterator keep it - and therefore its arena - alive.
//
// Entry layout in the arena:
//   varint32 internal_key_len | user_key | fixed64 tag | varint32 vlen | value
class MemTable {
  public:
    explicit MemTable(const InternalKeyComparator& cmp);
    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    void add(SequenceNumber seq, ValueType type, const Slice& user_key, const Slice& value);

    // If the memtable holds an entry for the key visible at lkey's snapshot,
    // returns true with *status = okay (+ *value) for a Put, or
    // *status = not_found for a tombstone. Returns false when this memtable
    // has nothing to say (probe the next source).
    bool get(const LookupKey& lkey, std::string* value, Status* status);

    // Yields internal keys, newest-first within a user key. The iterator
    // does NOT pin the memtable; callers hold a shared_ptr for its lifetime.
    Iterator* new_iterator();

    std::size_t approximate_memory_usage() const {
        return arena_.memory_usage();
    }

    bool empty() const {
        Table::Iterator it(&table_);
        it.seek_to_first();
        return !it.valid();
    }

    // WAL segment feeding this memtable; flushing it advances min_wal_number
    // past this. Set once by DBImpl at rotation.
    std::uint64_t wal_number = 0;

  private:
    friend class MemTableIterator;

    struct KeyComparator {
        const InternalKeyComparator* icmp;
        // Entries are length-prefixed internal keys.
        int operator()(const char* a, const char* b) const;
    };

    using Table = SkipList<const char*, KeyComparator>;

    const InternalKeyComparator icmp_;
    Arena arena_;
    Table table_;
};

} // namespace strata
