#pragma once

#include "db/dbformat.h"
#include "strata/status.h"
#include "strata/write_batch.h"

namespace strata {

class MemTable;

// Package-private surgery on WriteBatch::rep_ (header = 8B seq + 4B count).
struct WriteBatchInternal {
    static constexpr std::size_t kHeader = 12;

    static SequenceNumber sequence(const WriteBatch* b);
    static void set_sequence(WriteBatch* b, SequenceNumber seq);
    static std::uint32_t count(const WriteBatch* b);
    static void set_count(WriteBatch* b, std::uint32_t n);

    static Slice contents(const WriteBatch* b) {
        return Slice(b->rep_);
    }
    static void set_contents(WriteBatch* b, const Slice& contents);
    static std::size_t byte_size(const WriteBatch* b) {
        return b->rep_.size();
    }

    // Appends src's operations to dst (group commit coalescing).
    static void append(WriteBatch* dst, const WriteBatch* src);

    // Structural validation; WAL replay runs this before insert_into so a
    // CRC-valid but malformed record is rejected, never half-applied.
    static Status check(const Slice& contents);

    // Applies the batch with sequences sequence(b)..+count(b)-1.
    static Status insert_into(const WriteBatch* b, MemTable* mem);
};

} // namespace strata
