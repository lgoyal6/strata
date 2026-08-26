#pragma once

#include <cassert>
#include <cstdint>
#include <string>

#include "strata/slice.h"
#include "util/coding.h"

namespace strata {

inline constexpr int kNumLevels = 7;

using SequenceNumber = std::uint64_t;
// Sequences pack into the top 56 bits of the 8-byte tag.
inline constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;

enum ValueType : std::uint8_t {
    kTypeDeletion = 0x0,
    kTypeValue = 0x1,
};
// Tags sort descending, so the max type positions a seek at the newest
// entry <= the snapshot for a given user key.
inline constexpr ValueType kValueTypeForSeek = kTypeValue;

inline std::uint64_t pack_tag(SequenceNumber seq, ValueType type) {
    assert(seq <= kMaxSequenceNumber);
    return (seq << 8) | type;
}

// internal_key := user_key | fixed64 tag. Order: user_key asc, tag desc
// (newest version of a key first).
struct ParsedInternalKey {
    Slice user_key;
    SequenceNumber sequence = 0;
    ValueType type = kTypeValue;
};

void append_internal_key(std::string* dst, const Slice& user_key, SequenceNumber seq,
                         ValueType type);
bool parse_internal_key(const Slice& internal_key, ParsedInternalKey* result);

inline Slice extract_user_key(const Slice& internal_key) {
    assert(internal_key.size() >= 8);
    return Slice(internal_key.data(), internal_key.size() - 8);
}

inline std::uint64_t extract_tag(const Slice& internal_key) {
    assert(internal_key.size() >= 8);
    return decode_fixed64(internal_key.data() + internal_key.size() - 8);
}

class InternalKeyComparator {
  public:
    int compare(const Slice& a, const Slice& b) const;

    // For index-block separators: modifies *start to a short key in
    // [start, limit) / a short key >= *key, preserving internal-key order.
    void find_shortest_separator(std::string* start, const Slice& limit) const;
    void find_short_successor(std::string* key) const;
};

// Owned internal key (FileMeta bounds, index separators).
class InternalKey {
  public:
    InternalKey() = default;
    InternalKey(const Slice& user_key, SequenceNumber seq, ValueType type) {
        append_internal_key(&rep_, user_key, seq, type);
    }

    void decode_from(const Slice& s) {
        rep_.assign(s.data(), s.size());
    }
    Slice encoded() const {
        return rep_;
    }
    Slice user_key() const {
        return extract_user_key(rep_);
    }
    bool empty() const {
        return rep_.empty();
    }
    void clear() {
        rep_.clear();
    }

  private:
    std::string rep_;
};

// Memtable probe key. Layout: varint32(klen) | user_key | tag - so
// memtable_key() is a full skiplist entry prefix and internal_key() is the
// embedded internal key.
class LookupKey {
  public:
    LookupKey(const Slice& user_key, SequenceNumber snapshot_seq);
    LookupKey(const LookupKey&) = delete;
    LookupKey& operator=(const LookupKey&) = delete;
    ~LookupKey();

    Slice memtable_key() const {
        return Slice(start_, static_cast<std::size_t>(end_ - start_));
    }
    Slice internal_key() const {
        return Slice(kstart_, static_cast<std::size_t>(end_ - kstart_));
    }
    Slice user_key() const {
        return Slice(kstart_, static_cast<std::size_t>(end_ - kstart_) - 8);
    }

  private:
    const char* start_;
    const char* kstart_;
    const char* end_;
    char space_[200]; // avoids allocation for short keys
};

// --- file naming --------------------------------------------------------

enum class FileType {
    kWal,
    kTable,
    kManifest,
    kTempManifest,
    kLock,
    kUnknown,
};

std::string wal_file_name(const std::string& dbname, std::uint64_t number);
std::string table_file_name(const std::string& dbname, std::uint64_t number);
std::string manifest_file_name(const std::string& dbname);
std::string temp_manifest_file_name(const std::string& dbname);
std::string lock_file_name(const std::string& dbname);
// Parses a bare filename (no directory). Returns false for foreign files.
bool parse_file_name(const std::string& filename, std::uint64_t* number, FileType* type);

} // namespace strata
