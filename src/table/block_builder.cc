#include "table/block_builder.h"

#include <cassert>

#include "util/coding.h"

namespace strata {

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
    assert(restart_interval_ >= 1);
    restarts_.push_back(0); // first entry is always a restart
}

void BlockBuilder::reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_ = 0;
    finished_ = false;
    last_key_.clear();
}

void BlockBuilder::add(const Slice& key, const Slice& value) {
    assert(!finished_);
    std::size_t shared = 0;
    if (counter_ < restart_interval_) {
        const std::size_t min_len = last_key_.size() < key.size() ? last_key_.size() : key.size();
        while (shared < min_len && last_key_[shared] == key[shared]) {
            ++shared;
        }
    } else {
        restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
        counter_ = 0;
    }
    const std::size_t unshared = key.size() - shared;

    put_varint32(&buffer_, static_cast<std::uint32_t>(shared));
    put_varint32(&buffer_, static_cast<std::uint32_t>(unshared));
    put_varint32(&buffer_, static_cast<std::uint32_t>(value.size()));
    buffer_.append(key.data() + shared, unshared);
    buffer_.append(value.data(), value.size());

    last_key_.resize(shared);
    last_key_.append(key.data() + shared, unshared);
    assert(Slice(last_key_) == key);
    ++counter_;
}

Slice BlockBuilder::finish() {
    for (const std::uint32_t restart : restarts_) {
        put_fixed32(&buffer_, restart);
    }
    put_fixed32(&buffer_, static_cast<std::uint32_t>(restarts_.size()));
    finished_ = true;
    return Slice(buffer_);
}

} // namespace strata
