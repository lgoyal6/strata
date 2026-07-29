#pragma once

#include <memory>

#include "db/dbformat.h"
#include "strata/iterator.h"

namespace strata {

// Wraps an internal-key merging iterator into the user-facing view at a
// snapshot: newest visible version per user key, tombstones suppress
// everything older, invisible (seq > snapshot) entries filtered before type
// interpretation. Forward-only. `pin` keeps the sources (memtables, version)
// alive for the iterator's lifetime.
Iterator* new_db_iterator(std::unique_ptr<Iterator> internal, SequenceNumber snapshot_seq,
                          std::shared_ptr<void> pin);

} // namespace strata
