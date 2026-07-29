#include "strata/write_batch.h"

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace strata {

WriteBatch::WriteBatch() {
    clear();
}

void WriteBatch::clear() {
    rep_.clear();
    rep_.resize(WriteBatchInternal::kHeader, '\0');
    user_bytes_ = 0;
}

std::uint32_t WriteBatch::count() const {
    return WriteBatchInternal::count(this);
}

void WriteBatch::put(const Slice& key, const Slice& value) {
    WriteBatchInternal::set_count(this, WriteBatchInternal::count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeValue));
    put_length_prefixed_slice(&rep_, key);
    put_length_prefixed_slice(&rep_, value);
    user_bytes_ += key.size() + value.size();
}

void WriteBatch::remove(const Slice& key) {
    WriteBatchInternal::set_count(this, WriteBatchInternal::count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeDeletion));
    put_length_prefixed_slice(&rep_, key);
    user_bytes_ += key.size();
}

SequenceNumber WriteBatchInternal::sequence(const WriteBatch* b) {
    return decode_fixed64(b->rep_.data());
}

void WriteBatchInternal::set_sequence(WriteBatch* b, SequenceNumber seq) {
    encode_fixed64(b->rep_.data(), seq);
}

std::uint32_t WriteBatchInternal::count(const WriteBatch* b) {
    return decode_fixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::set_count(WriteBatch* b, std::uint32_t n) {
    encode_fixed32(b->rep_.data() + 8, n);
}

void WriteBatchInternal::set_contents(WriteBatch* b, const Slice& contents) {
    assert(contents.size() >= kHeader);
    b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::append(WriteBatch* dst, const WriteBatch* src) {
    set_count(dst, count(dst) + count(src));
    assert(src->rep_.size() >= kHeader);
    dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
    dst->user_bytes_ += src->user_bytes_;
}

Status WriteBatchInternal::check(const Slice& contents) {
    if (contents.size() < kHeader) {
        return Status::corruption("batch too small");
    }
    Slice input(contents.data() + kHeader, contents.size() - kHeader);
    const std::uint32_t expected = decode_fixed32(contents.data() + 8);
    std::uint32_t found = 0;
    while (!input.empty()) {
        const auto op = static_cast<std::uint8_t>(input[0]);
        input.remove_prefix(1);
        Slice key, value;
        switch (op) {
        case kTypeValue:
            if (!get_length_prefixed_slice(&input, &key) ||
                !get_length_prefixed_slice(&input, &value)) {
                return Status::corruption("bad batch put");
            }
            break;
        case kTypeDeletion:
            if (!get_length_prefixed_slice(&input, &key)) {
                return Status::corruption("bad batch delete");
            }
            break;
        default:
            return Status::corruption("unknown batch op");
        }
        ++found;
    }
    if (found != expected) {
        return Status::corruption("batch count mismatch");
    }
    return Status::okay();
}

Status WriteBatchInternal::insert_into(const WriteBatch* b, MemTable* mem) {
    Slice input = contents(b);
    SequenceNumber seq = sequence(b);
    input.remove_prefix(kHeader);
    while (!input.empty()) {
        const auto op = static_cast<std::uint8_t>(input[0]);
        input.remove_prefix(1);
        Slice key, value;
        switch (op) {
        case kTypeValue:
            if (!get_length_prefixed_slice(&input, &key) ||
                !get_length_prefixed_slice(&input, &value)) {
                return Status::corruption("bad batch put");
            }
            mem->add(seq, kTypeValue, key, value);
            break;
        case kTypeDeletion:
            if (!get_length_prefixed_slice(&input, &key)) {
                return Status::corruption("bad batch delete");
            }
            mem->add(seq, kTypeDeletion, key, Slice());
            break;
        default:
            return Status::corruption("unknown batch op");
        }
        ++seq;
    }
    return Status::okay();
}

} // namespace strata
