#pragma once

#include <memory>
#include <vector>

#include "db/dbformat.h"
#include "strata/iterator.h"

namespace strata {

// Merges N sorted children into one sorted stream of internal keys.
// Internal keys are globally unique, but ties break toward the
// lower-indexed (newer) child for determinism. Forward-only.
Iterator* new_merging_iterator(const InternalKeyComparator* cmp,
                               std::vector<std::unique_ptr<Iterator>> children);

} // namespace strata
