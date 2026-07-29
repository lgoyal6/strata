#include "table/table_builder.h"

#include <cassert>

#include "table/bloom.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {

TableBuilder::TableBuilder(const Options& options, const InternalKeyComparator& icmp,
                           WritableFile* file)
    : options_(options), icmp_(icmp), file_(file), data_block_(options.block_restart_interval),
      index_block_(1) {} // index entries are incompressible separators

void TableBuilder::add(const Slice& internal_key, const Slice& value) {
    if (!status_.ok()) {
        return;
    }
    assert(num_entries_ == 0 || icmp_.compare(internal_key, Slice(last_key_)) > 0);

    if (pending_index_entry_) {
        // last_key_ still holds the previous block's last key; shorten it to
        // a separator < internal_key so the index stays small.
        icmp_.find_shortest_separator(&last_key_, internal_key);
        std::string handle_encoding;
        pending_handle_.encode_to(&handle_encoding);
        index_block_.add(Slice(last_key_), Slice(handle_encoding));
        pending_index_entry_ = false;
    }

    if (options_.bloom_bits_per_key > 0) {
        filter_hashes_.push_back(bloom_hash(extract_user_key(internal_key)));
    }
    last_key_.assign(internal_key.data(), internal_key.size());
    ++num_entries_;
    data_block_.add(internal_key, value);

    if (data_block_.current_size_estimate() >= options_.block_size) {
        flush_data_block();
    }
}

void TableBuilder::flush_data_block() {
    if (data_block_.empty() || !status_.ok()) {
        return;
    }
    write_raw_block(data_block_.finish(), &pending_handle_);
    data_block_.reset();
    pending_index_entry_ = true;
}

void TableBuilder::write_raw_block(const Slice& contents, BlockHandle* handle) {
    handle->offset = offset_;
    handle->size = contents.size();

    Status s = file_->append(contents);
    if (s.ok()) {
        char trailer[kBlockTrailerSize];
        trailer[0] = static_cast<char>(kNoCompression);
        std::uint32_t crc = crc32c(contents.data(), contents.size());
        crc = crc32c_extend(crc, trailer, 1); // covers the compression byte
        encode_fixed32(trailer + 1, crc32c_mask(crc));
        s = file_->append(Slice(trailer, sizeof(trailer)));
    }
    if (s.ok()) {
        offset_ += contents.size() + kBlockTrailerSize;
    } else {
        status_ = s;
    }
}

Status TableBuilder::finish() {
    flush_data_block();
    if (!status_.ok()) {
        return status_;
    }

    Footer footer;

    if (options_.bloom_bits_per_key > 0) {
        std::string filter;
        bloom_build(filter_hashes_, options_.bloom_bits_per_key, &filter);
        write_raw_block(Slice(filter), &footer.filter_handle);
    }

    if (pending_index_entry_) {
        icmp_.find_short_successor(&last_key_);
        std::string handle_encoding;
        pending_handle_.encode_to(&handle_encoding);
        index_block_.add(Slice(last_key_), Slice(handle_encoding));
        pending_index_entry_ = false;
    }
    write_raw_block(index_block_.finish(), &footer.index_handle);

    if (status_.ok()) {
        std::string footer_encoding;
        footer.encode_to(&footer_encoding);
        status_ = file_->append(Slice(footer_encoding));
        if (status_.ok()) {
            offset_ += footer_encoding.size();
        }
    }
    return status_;
}

} // namespace strata
