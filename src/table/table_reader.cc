#include "table/table_reader.h"

#include "table/bloom.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace strata {

Status TableReader::open(const Options& options, const InternalKeyComparator& icmp,
                         std::unique_ptr<RandomAccessFile> file, std::uint64_t file_number,
                         std::uint64_t file_size, BlockCache* block_cache, TableReadStats* stats,
                         std::shared_ptr<TableReader>* out) {
    if (file_size < kFooterSize) {
        return Status::corruption("file too short for footer");
    }
    char footer_space[kFooterSize];
    Slice footer_input;
    Status s = file->read(file_size - kFooterSize, kFooterSize, &footer_input, footer_space);
    if (!s.ok()) {
        return s;
    }
    Footer footer;
    s = footer.decode_from(footer_input);
    if (!s.ok()) {
        return s;
    }

    auto reader = std::shared_ptr<TableReader>(new TableReader(
        options, icmp, std::move(file), file_number, file_size, block_cache, stats));

    s = reader->read_block(footer.index_handle, &reader->index_block_);
    if (!s.ok()) {
        return s;
    }
    if (footer.filter_handle.size > 0) {
        // The filter is not entry-structured: CRC-check it and keep raw bytes.
        s = reader->read_raw(footer.filter_handle, &reader->filter_);
        if (!s.ok()) {
            return s;
        }
    }
    *out = std::move(reader);
    return Status::okay();
}

Status TableReader::read_raw(const BlockHandle& handle, std::string* out) const {
    if (handle.offset + handle.size + kBlockTrailerSize > file_size_ ||
        handle.offset + handle.size < handle.offset) {
        return Status::corruption("block handle out of range");
    }
    const std::size_t n = static_cast<std::size_t>(handle.size) + kBlockTrailerSize;
    std::string buf;
    buf.resize(n);
    Slice contents;
    Status s = file_->read(handle.offset, n, &contents, buf.data());
    if (!s.ok()) {
        return s;
    }
    if (contents.size() != n) {
        return Status::corruption("truncated block read");
    }
    const char* data = contents.data();
    const std::uint32_t expected = crc32c_unmask(decode_fixed32(data + handle.size + 1));
    const std::uint32_t actual = crc32c(data, static_cast<std::size_t>(handle.size) + 1);
    if (actual != expected) {
        return Status::corruption("block checksum mismatch");
    }
    if (static_cast<std::uint8_t>(data[handle.size]) != kNoCompression) {
        return Status::corruption("unknown block compression");
    }
    out->assign(data, static_cast<std::size_t>(handle.size));
    return Status::okay();
}

Status TableReader::read_block(const BlockHandle& handle, std::shared_ptr<const Block>* out) const {
    const std::uint64_t cache_key = BlockCache::make_key(file_number_, handle.offset);
    if (block_cache_ != nullptr) {
        if (auto cached = block_cache_->lookup(cache_key)) {
            auto block = std::make_shared<const Block>(std::move(cached));
            if (block->malformed()) {
                return Status::corruption("malformed block (cached)");
            }
            *out = std::move(block);
            return Status::okay();
        }
    }
    auto contents = std::make_shared<std::string>();
    Status s = read_raw(handle, contents.get());
    if (!s.ok()) {
        return s;
    }
    if (block_cache_ != nullptr) {
        block_cache_->insert(cache_key, contents);
    }
    auto block =
        std::make_shared<const Block>(std::shared_ptr<const std::string>(std::move(contents)));
    if (block->malformed()) {
        return Status::corruption("malformed block");
    }
    *out = std::move(block);
    return Status::okay();
}

Status TableReader::get(const Slice& internal_key, std::string* found_ikey,
                        std::string* found_value, bool* found) {
    *found = false;
    if (!filter_.empty()) {
        if (stats_ != nullptr) {
            stats_->bloom_checks.fetch_add(1, std::memory_order_relaxed);
        }
        if (!bloom_may_contain(Slice(filter_), bloom_hash(extract_user_key(internal_key)))) {
            if (stats_ != nullptr) {
                stats_->bloom_skips.fetch_add(1, std::memory_order_relaxed);
            }
            return Status::okay();
        }
    }

    const std::unique_ptr<Iterator> index_iter(Block::new_iterator(index_block_, &icmp_));
    index_iter->seek(internal_key);
    if (!index_iter->valid()) {
        return index_iter->status();
    }
    BlockHandle handle;
    Slice handle_value = index_iter->value();
    if (!handle.decode_from(&handle_value)) {
        return Status::corruption("bad index entry");
    }
    std::shared_ptr<const Block> data_block;
    Status s = read_block(handle, &data_block);
    if (!s.ok()) {
        return s;
    }
    const std::unique_ptr<Iterator> data_iter(Block::new_iterator(std::move(data_block), &icmp_));
    data_iter->seek(internal_key);
    if (!data_iter->valid()) {
        return data_iter->status();
    }
    const Slice entry_key = data_iter->key();
    if (entry_key.size() < 8 || extract_user_key(entry_key) != extract_user_key(internal_key)) {
        return Status::okay(); // different user key: nothing here
    }
    found_ikey->assign(entry_key.data(), entry_key.size());
    found_value->assign(data_iter->value().data(), data_iter->value().size());
    *found = true;
    return Status::okay();
}

// Two-level iterator: an index cursor plus the current data-block cursor.
class TableReader::Iter final : public Iterator {
  public:
    explicit Iter(std::shared_ptr<TableReader> table)
        : table_(std::move(table)),
          index_iter_(Block::new_iterator(table_->index_block_, &table_->icmp_)) {}

    bool valid() const override {
        return data_iter_ != nullptr && data_iter_->valid();
    }

    void seek_to_first() override {
        index_iter_->seek_to_first();
        init_data_block();
        if (data_iter_ != nullptr) {
            data_iter_->seek_to_first();
        }
        skip_empty_blocks();
    }

    void seek(const Slice& target) override {
        index_iter_->seek(target);
        init_data_block();
        if (data_iter_ != nullptr) {
            data_iter_->seek(target);
        }
        skip_empty_blocks();
    }

    void next() override {
        assert(valid());
        data_iter_->next();
        skip_empty_blocks();
    }

    Slice key() const override {
        return data_iter_->key();
    }
    Slice value() const override {
        return data_iter_->value();
    }

    Status status() const override {
        if (!status_.ok()) {
            return status_;
        }
        if (!index_iter_->status().ok()) {
            return index_iter_->status();
        }
        if (data_iter_ != nullptr && !data_iter_->status().ok()) {
            return data_iter_->status();
        }
        return Status::okay();
    }

  private:
    void init_data_block() {
        data_iter_.reset();
        if (!index_iter_->valid()) {
            return;
        }
        BlockHandle handle;
        Slice handle_value = index_iter_->value();
        if (!handle.decode_from(&handle_value)) {
            status_ = Status::corruption("bad index entry");
            return;
        }
        std::shared_ptr<const Block> block;
        const Status s = table_->read_block(handle, &block);
        if (!s.ok()) {
            status_ = s;
            return;
        }
        data_iter_.reset(Block::new_iterator(std::move(block), &table_->icmp_));
    }

    void skip_empty_blocks() {
        while (status_.ok() && (data_iter_ == nullptr || !data_iter_->valid())) {
            if (data_iter_ != nullptr && !data_iter_->status().ok()) {
                status_ = data_iter_->status();
                return;
            }
            if (!index_iter_->valid()) {
                data_iter_.reset();
                return;
            }
            index_iter_->next();
            init_data_block();
            if (data_iter_ != nullptr) {
                data_iter_->seek_to_first();
            }
        }
    }

    std::shared_ptr<TableReader> table_;
    std::unique_ptr<Iterator> index_iter_;
    std::unique_ptr<Iterator> data_iter_;
    Status status_;
};

Iterator* TableReader::new_iterator() {
    return new Iter(shared_from_this());
}

} // namespace strata
