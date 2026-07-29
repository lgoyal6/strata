#include "db/dbformat.h"

#include <cstdio>
#include <cstring>

namespace strata {

void append_internal_key(std::string* dst, const Slice& user_key, SequenceNumber seq,
                         ValueType type) {
    dst->append(user_key.data(), user_key.size());
    put_fixed64(dst, pack_tag(seq, type));
}

bool parse_internal_key(const Slice& internal_key, ParsedInternalKey* result) {
    if (internal_key.size() < 8) {
        return false;
    }
    const std::uint64_t tag = extract_tag(internal_key);
    const auto type_byte = static_cast<std::uint8_t>(tag & 0xffu);
    if (type_byte > kTypeValue) {
        return false;
    }
    result->user_key = extract_user_key(internal_key);
    result->sequence = tag >> 8;
    result->type = static_cast<ValueType>(type_byte);
    return true;
}

int InternalKeyComparator::compare(const Slice& a, const Slice& b) const {
    const int r = extract_user_key(a).compare(extract_user_key(b));
    if (r != 0) {
        return r;
    }
    const std::uint64_t atag = extract_tag(a);
    const std::uint64_t btag = extract_tag(b);
    if (atag > btag) {
        return -1; // larger tag (newer) sorts first
    }
    if (atag < btag) {
        return +1;
    }
    return 0;
}

namespace {

// Bytewise shortest-separator on user keys: truncate to the first byte that
// can be incremented while staying < limit.
void user_shortest_separator(std::string* start, const Slice& limit) {
    const std::size_t min_len = start->size() < limit.size() ? start->size() : limit.size();
    std::size_t diff = 0;
    while (diff < min_len && (*start)[diff] == limit[diff]) {
        ++diff;
    }
    if (diff >= min_len) {
        return; // one is a prefix of the other; leave *start alone
    }
    const auto start_byte = static_cast<std::uint8_t>((*start)[diff]);
    const auto limit_byte = static_cast<std::uint8_t>(limit[diff]);
    if (start_byte < 0xffu && start_byte + 1 < limit_byte) {
        (*start)[diff] = static_cast<char>(start_byte + 1);
        start->resize(diff + 1);
    }
}

void user_short_successor(std::string* key) {
    for (std::size_t i = 0; i < key->size(); ++i) {
        const auto byte = static_cast<std::uint8_t>((*key)[i]);
        if (byte != 0xffu) {
            (*key)[i] = static_cast<char>(byte + 1);
            key->resize(i + 1);
            return;
        }
    }
    // All 0xff: leave unchanged (already a maximal run).
}

} // namespace

void InternalKeyComparator::find_shortest_separator(std::string* start, const Slice& limit) const {
    std::string user_start(extract_user_key(Slice(*start)).to_string());
    const Slice user_limit = extract_user_key(limit);
    std::string tmp = user_start;
    user_shortest_separator(&tmp, user_limit);
    if (tmp.size() < user_start.size() && Slice(user_start).compare(tmp) < 0) {
        // A shorter physical separator: give it the max tag so it sorts
        // before every real entry with that user key.
        put_fixed64(&tmp, pack_tag(kMaxSequenceNumber, kValueTypeForSeek));
        assert(compare(Slice(*start), tmp) < 0);
        assert(compare(Slice(tmp), limit) < 0);
        start->swap(tmp);
    }
}

void InternalKeyComparator::find_short_successor(std::string* key) const {
    std::string user_key(extract_user_key(Slice(*key)).to_string());
    std::string tmp = user_key;
    user_short_successor(&tmp);
    if (tmp.size() < user_key.size() && Slice(user_key).compare(tmp) < 0) {
        put_fixed64(&tmp, pack_tag(kMaxSequenceNumber, kValueTypeForSeek));
        assert(compare(Slice(*key), tmp) < 0);
        key->swap(tmp);
    }
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber snapshot_seq) {
    const std::size_t usize = user_key.size();
    const std::size_t needed = usize + 13; // varint32 (max 5) + key + tag
    char* dst = needed <= sizeof(space_) ? space_ : new char[needed];
    start_ = dst;
    dst = encode_varint32(dst, static_cast<std::uint32_t>(usize + 8));
    kstart_ = dst;
    std::memcpy(dst, user_key.data(), usize);
    dst += usize;
    encode_fixed64(dst, pack_tag(snapshot_seq, kValueTypeForSeek));
    dst += 8;
    end_ = dst;
}

LookupKey::~LookupKey() {
    if (start_ != space_) {
        delete[] start_;
    }
}

std::string wal_file_name(const std::string& dbname, std::uint64_t number) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/%06llu.wal", static_cast<unsigned long long>(number));
    return dbname + buf;
}

std::string table_file_name(const std::string& dbname, std::uint64_t number) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/%06llu.sst", static_cast<unsigned long long>(number));
    return dbname + buf;
}

std::string manifest_file_name(const std::string& dbname) {
    return dbname + "/MANIFEST";
}

std::string temp_manifest_file_name(const std::string& dbname) {
    return dbname + "/MANIFEST.tmp";
}

std::string lock_file_name(const std::string& dbname) {
    return dbname + "/LOCK";
}

bool parse_file_name(const std::string& filename, std::uint64_t* number, FileType* type) {
    *number = 0;
    if (filename == "MANIFEST") {
        *type = FileType::kManifest;
        return true;
    }
    if (filename == "MANIFEST.tmp") {
        *type = FileType::kTempManifest;
        return true;
    }
    if (filename == "LOCK") {
        *type = FileType::kLock;
        return true;
    }
    // NNNNNN.wal / NNNNNN.sst
    const std::size_t dot = filename.find('.');
    if (dot == std::string::npos || dot == 0) {
        *type = FileType::kUnknown;
        return false;
    }
    std::uint64_t num = 0;
    for (std::size_t i = 0; i < dot; ++i) {
        const char c = filename[i];
        if (c < '0' || c > '9') {
            *type = FileType::kUnknown;
            return false;
        }
        num = num * 10 + static_cast<std::uint64_t>(c - '0');
    }
    const std::string ext = filename.substr(dot);
    if (ext == ".wal") {
        *type = FileType::kWal;
    } else if (ext == ".sst") {
        *type = FileType::kTable;
    } else {
        *type = FileType::kUnknown;
        return false;
    }
    *number = num;
    return true;
}

} // namespace strata
