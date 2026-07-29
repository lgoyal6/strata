#include "table/block.h"

#include "util/coding.h"

namespace strata {

Block::Block(std::shared_ptr<const std::string> contents) : contents_(std::move(contents)) {
    const std::size_t size = contents_->size();
    if (size < 4) {
        malformed_ = true;
        return;
    }
    num_restarts_ = decode_fixed32(data() + size - 4);
    // Each restart is 4 bytes; they plus the count must fit in the block.
    const std::uint32_t max_restarts = static_cast<std::uint32_t>((size - 4) / 4);
    if (num_restarts_ == 0 || num_restarts_ > max_restarts) {
        malformed_ = true;
        return;
    }
    restarts_offset_ = static_cast<std::uint32_t>(size - 4 - 4ull * num_restarts_);
}

class Block::Iter final : public Iterator {
  public:
    Iter(std::shared_ptr<const Block> block, const InternalKeyComparator* cmp)
        : block_(std::move(block)), cmp_(cmp) {}

    bool valid() const override {
        return valid_;
    }

    void seek_to_first() override {
        if (block_->malformed_) {
            corrupt();
            return;
        }
        seek_to_restart(0);
        parse_next_entry();
    }

    void seek(const Slice& target) override {
        if (block_->malformed_) {
            corrupt();
            return;
        }
        // Binary search the restart array for the last restart whose key < target.
        std::uint32_t left = 0;
        std::uint32_t right = block_->num_restarts_ - 1;
        while (left < right) {
            const std::uint32_t mid = (left + right + 1) / 2;
            Slice mid_key;
            if (!key_at_restart(mid, &mid_key)) {
                corrupt();
                return;
            }
            if (cmp_->compare(mid_key, target) < 0) {
                left = mid;
            } else {
                right = mid - 1;
            }
        }
        seek_to_restart(left);
        // Linear scan forward to the first entry >= target.
        while (true) {
            if (!parse_next_entry()) {
                return; // hit the end (or corruption); valid_ already false
            }
            if (cmp_->compare(key(), target) >= 0) {
                return;
            }
        }
    }

    void next() override {
        assert(valid_);
        parse_next_entry();
    }

    Slice key() const override {
        assert(valid_);
        return Slice(key_);
    }

    Slice value() const override {
        assert(valid_);
        return value_;
    }

    Status status() const override {
        return status_;
    }

  private:
    void corrupt() {
        valid_ = false;
        status_ = Status::corruption("malformed block");
    }

    const char* data_begin() const {
        return block_->data();
    }
    const char* restarts_begin() const {
        return block_->data() + block_->restarts_offset_;
    }

    std::uint32_t restart_point(std::uint32_t i) const {
        return decode_fixed32(restarts_begin() + 4 * i);
    }

    // Decodes the full key stored at restart i (shared_len must be 0).
    bool key_at_restart(std::uint32_t i, Slice* out) {
        const std::uint32_t offset = restart_point(i);
        if (offset >= block_->restarts_offset_) {
            return false;
        }
        const char* p = data_begin() + offset;
        const char* limit = data_begin() + block_->restarts_offset_;
        std::uint32_t shared = 0, unshared = 0, value_len = 0;
        p = get_varint32_ptr(p, limit, &shared);
        if (p != nullptr) {
            p = get_varint32_ptr(p, limit, &unshared);
        }
        if (p != nullptr) {
            p = get_varint32_ptr(p, limit, &value_len);
        }
        if (p == nullptr || shared != 0 || unshared > static_cast<std::size_t>(limit - p)) {
            return false;
        }
        *out = Slice(p, unshared);
        return true;
    }

    void seek_to_restart(std::uint32_t i) {
        key_.clear();
        value_ = Slice();
        const std::uint32_t offset = restart_point(i);
        next_entry_offset_ = offset;
        valid_ = false; // becomes valid after parse_next_entry()
    }

    // Parses the entry at next_entry_offset_ into key_/value_.
    bool parse_next_entry() {
        const char* p = data_begin() + next_entry_offset_;
        const char* limit = data_begin() + block_->restarts_offset_;
        if (p >= limit) {
            valid_ = false; // clean end of block
            return false;
        }
        std::uint32_t shared = 0, unshared = 0, value_len = 0;
        p = get_varint32_ptr(p, limit, &shared);
        if (p != nullptr) {
            p = get_varint32_ptr(p, limit, &unshared);
        }
        if (p != nullptr) {
            p = get_varint32_ptr(p, limit, &value_len);
        }
        if (p == nullptr || shared > key_.size() ||
            unshared + value_len > static_cast<std::size_t>(limit - p)) {
            corrupt();
            return false;
        }
        key_.resize(shared);
        key_.append(p, unshared);
        value_ = Slice(p + unshared, value_len);
        next_entry_offset_ = static_cast<std::uint32_t>(p + unshared + value_len - data_begin());
        valid_ = true;
        return true;
    }

    std::shared_ptr<const Block> block_;
    const InternalKeyComparator* cmp_;
    std::uint32_t next_entry_offset_ = 0;
    std::string key_; // owned: reconstructed from prefix compression
    Slice value_;
    bool valid_ = false;
    Status status_;
};

Iterator* Block::new_iterator(std::shared_ptr<const Block> block,
                              const InternalKeyComparator* cmp) {
    return new Iter(std::move(block), cmp);
}

} // namespace strata
