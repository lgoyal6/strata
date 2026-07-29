#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

namespace strata {

// A pointer + length view of external storage. The caller guarantees the
// underlying bytes outlive the Slice (same contract as leveldb::Slice).
class Slice {
  public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* d, std::size_t n) : data_(d), size_(n) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
    Slice(const char* s) : data_(s), size_(std::strlen(s)) {}

    const char* data() const {
        return data_;
    }
    std::size_t size() const {
        return size_;
    }
    bool empty() const {
        return size_ == 0;
    }

    char operator[](std::size_t i) const {
        assert(i < size_);
        return data_[i];
    }

    void clear() {
        data_ = "";
        size_ = 0;
    }

    void remove_prefix(std::size_t n) {
        assert(n <= size_);
        data_ += n;
        size_ -= n;
    }

    std::string to_string() const {
        return std::string(data_, size_);
    }

    // <0, ==0, >0 as in memcmp; shorter operand sorts first on shared prefix.
    int compare(const Slice& b) const {
        const std::size_t min_len = size_ < b.size_ ? size_ : b.size_;
        int r = std::memcmp(data_, b.data_, min_len);
        if (r == 0) {
            if (size_ < b.size_) {
                r = -1;
            } else if (size_ > b.size_) {
                r = 1;
            }
        }
        return r;
    }

    bool starts_with(const Slice& prefix) const {
        return size_ >= prefix.size_ && std::memcmp(data_, prefix.data_, prefix.size_) == 0;
    }

  private:
    const char* data_;
    std::size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}
inline bool operator!=(const Slice& a, const Slice& b) {
    return !(a == b);
}

} // namespace strata
