#include "db/memtable.h"

#include <cstring>

#include "util/coding.h"

namespace strata {
namespace {

// Decodes the length-prefixed internal key at the start of an entry.
Slice entry_internal_key(const char* entry) {
    std::uint32_t len = 0;
    const char* p = get_varint32_ptr(entry, entry + 5, &len);
    return Slice(p, len);
}

} // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
    return icmp->compare(entry_internal_key(a), entry_internal_key(b));
}

MemTable::MemTable(const InternalKeyComparator& cmp)
    : icmp_(cmp), table_(KeyComparator{&icmp_}, &arena_) {}

void MemTable::add(SequenceNumber seq, ValueType type, const Slice& user_key, const Slice& value) {
    const std::size_t ikey_size = user_key.size() + 8;
    const std::size_t encoded_len = static_cast<std::size_t>(varint_length(ikey_size)) + ikey_size +
                                    static_cast<std::size_t>(varint_length(value.size())) +
                                    value.size();
    char* buf = arena_.allocate(encoded_len);
    char* p = encode_varint32(buf, static_cast<std::uint32_t>(ikey_size));
    std::memcpy(p, user_key.data(), user_key.size());
    p += user_key.size();
    encode_fixed64(p, pack_tag(seq, type));
    p += 8;
    p = encode_varint32(p, static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) {
        std::memcpy(p, value.data(), value.size());
        p += value.size();
    }
    assert(static_cast<std::size_t>(p - buf) == encoded_len);
    table_.insert(buf);
}

bool MemTable::get(const LookupKey& lkey, std::string* value, Status* status) {
    Table::Iterator iter(&table_);
    iter.seek(lkey.memtable_key().data());
    if (!iter.valid()) {
        return false;
    }
    // The seek key has tag (snapshot, kValueTypeForSeek); iter is now at the
    // first entry with the same user key and seq <= snapshot, or at a
    // different user key entirely.
    const char* entry = iter.key();
    const Slice ikey = entry_internal_key(entry);
    if (extract_user_key(ikey) != lkey.user_key()) {
        return false;
    }
    const std::uint64_t tag = extract_tag(ikey);
    switch (static_cast<ValueType>(tag & 0xffu)) {
    case kTypeValue: {
        const char* vstart = ikey.data() + ikey.size();
        std::uint32_t vlen = 0;
        const char* vp = get_varint32_ptr(vstart, vstart + 5, &vlen);
        value->assign(vp, vlen);
        *status = Status::okay();
        return true;
    }
    case kTypeDeletion:
        *status = Status::not_found();
        return true;
    }
    return false;
}

// Not in an anonymous namespace: must match MemTable's friend declaration.
class MemTableIterator final : public Iterator {
  public:
    explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

    bool valid() const override {
        return iter_.valid();
    }

    void seek_to_first() override {
        iter_.seek_to_first();
    }

    void seek(const Slice& target) override {
        // Encode the internal-key target as a memtable entry prefix.
        scratch_.clear();
        put_varint32(&scratch_, static_cast<std::uint32_t>(target.size()));
        scratch_.append(target.data(), target.size());
        iter_.seek(scratch_.data());
    }

    void next() override {
        iter_.next();
    }

    Slice key() const override {
        return entry_internal_key(iter_.key());
    }

    Slice value() const override {
        const Slice k = key();
        const char* p = k.data() + k.size();
        std::uint32_t vlen = 0;
        const char* vp = get_varint32_ptr(p, p + 5, &vlen);
        return Slice(vp, vlen);
    }

    Status status() const override {
        return Status::okay();
    }

  private:
    MemTable::Table::Iterator iter_;
    std::string scratch_;
};

Iterator* MemTable::new_iterator() {
    return new MemTableIterator(&table_);
}

} // namespace strata
