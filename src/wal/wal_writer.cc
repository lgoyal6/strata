#include "wal/wal_writer.h"

#include "util/coding.h"
#include "util/crc32c.h"
#include "wal/wal_format.h"

namespace strata {

Status WalWriter::create(Env* env, const std::string& fname, std::uint64_t db_uuid,
                         std::unique_ptr<WalWriter>* out) {
    std::unique_ptr<WritableFile> file;
    Status s = env->new_writable_file(fname, &file);
    if (!s.ok()) {
        return s;
    }
    char header[kWalFileHeaderSize];
    encode_fixed64(header, kWalMagic);
    encode_fixed64(header + 8, db_uuid);
    s = file->append(Slice(header, sizeof(header)));
    if (s.ok()) {
        s = file->flush();
    }
    if (!s.ok()) {
        return s;
    }
    out->reset(new WalWriter(std::move(file)));
    (*out)->bytes_appended_ = kWalFileHeaderSize;
    return Status::okay();
}

Status WalWriter::add_record(const Slice& payload) {
    std::lock_guard<std::mutex> lock(mu_);
    char header[kWalRecordHeaderSize];
    const std::uint8_t type = kWalFullBatch;
    std::uint32_t crc = crc32c(&type, 1);
    crc = crc32c_extend(crc, payload.data(), payload.size());
    encode_fixed32(header, crc32c_mask(crc));
    encode_fixed32(header + 4, static_cast<std::uint32_t>(payload.size()));
    header[8] = static_cast<char>(type);

    Status s = file_->append(Slice(header, sizeof(header)));
    if (s.ok()) {
        s = file_->append(payload);
    }
    if (s.ok()) {
        // Kernel visibility before the commit is acknowledged: "fsync=never"
        // must still survive SIGKILL (docs/DESIGN.md §1.2).
        s = file_->flush();
    }
    if (s.ok()) {
        bytes_appended_ += sizeof(header) + payload.size();
    }
    return s;
}

} // namespace strata
