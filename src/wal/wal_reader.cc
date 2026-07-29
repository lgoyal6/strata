#include "wal/wal_reader.h"

#include "util/coding.h"
#include "util/crc32c.h"
#include "wal/wal_format.h"

namespace strata {

Status WalReader::open(Env* env, const std::string& fname, std::uint64_t expected_uuid,
                       std::unique_ptr<WalReader>* out) {
    std::uint64_t file_size = 0;
    Status s = env->get_file_size(fname, &file_size);
    if (!s.ok()) {
        return s;
    }
    std::unique_ptr<SequentialFile> file;
    s = env->new_sequential_file(fname, &file);
    if (!s.ok()) {
        return s;
    }
    auto reader = std::unique_ptr<WalReader>(new WalReader(std::move(file), file_size));

    if (file_size < kWalFileHeaderSize) {
        // A WAL torn inside its own header holds no records by construction.
        reader->done_ = true;
        reader->truncated_tail_ = file_size > 0;
        reader->remaining_ = 0;
        *out = std::move(reader);
        return Status::okay();
    }
    std::string header;
    if (!reader->read_fully(kWalFileHeaderSize, &header)) {
        return Status::io_error(fname + ": short read on WAL header");
    }
    if (decode_fixed64(header.data()) != kWalMagic) {
        return Status::corruption(fname + ": bad WAL magic");
    }
    reader->db_uuid_ = decode_fixed64(header.data() + 8);
    if (expected_uuid != 0 && reader->db_uuid_ != expected_uuid) {
        return Status::corruption(fname + ": WAL from a different database generation");
    }
    reader->remaining_ = file_size - kWalFileHeaderSize;
    reader->valid_offset_ = kWalFileHeaderSize;
    *out = std::move(reader);
    return Status::okay();
}

bool WalReader::read_fully(std::size_t n, std::string* out) {
    out->clear();
    out->resize(n);
    std::size_t got = 0;
    while (got < n) {
        Slice chunk;
        const Status s = file_->read(n - got, &chunk, out->data() + got);
        if (!s.ok() || chunk.empty()) {
            return false;
        }
        got += chunk.size();
    }
    return true;
}

bool WalReader::read_record(std::string* record) {
    if (done_) {
        return false;
    }
    if (remaining_ == 0) {
        done_ = true;
        return false; // clean EOF
    }
    if (remaining_ < kWalRecordHeaderSize) {
        done_ = true;
        truncated_tail_ = true; // torn mid-header
        return false;
    }
    std::string header;
    if (!read_fully(kWalRecordHeaderSize, &header)) {
        done_ = true;
        truncated_tail_ = true;
        return false;
    }
    remaining_ -= kWalRecordHeaderSize;

    const std::uint32_t stored_crc = crc32c_unmask(decode_fixed32(header.data()));
    const std::uint32_t length = decode_fixed32(header.data() + 4);
    const auto type = static_cast<std::uint8_t>(header[8]);

    // A torn length field can decode arbitrarily large: bounds-check against
    // the remaining file before any allocation or read.
    if (length > remaining_) {
        done_ = true;
        truncated_tail_ = true;
        return false;
    }
    if (!read_fully(length, record)) {
        done_ = true;
        truncated_tail_ = true;
        return false;
    }
    remaining_ -= length;

    std::uint32_t actual = crc32c(&type, 1);
    actual = crc32c_extend(actual, record->data(), record->size());
    if (actual != stored_crc || type != kWalFullBatch) {
        done_ = true;
        truncated_tail_ = true;
        record->clear();
        return false;
    }
    valid_offset_ += kWalRecordHeaderSize + record->size();
    return true;
}

} // namespace strata
