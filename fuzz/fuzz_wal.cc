// libFuzzer target: arbitrary bytes as a WAL file. The reader must parse or
// reject — never crash, over-read, or accept a record whose CRC does not
// match. Records that do parse must satisfy the WriteBatch structural check
// contract the recovery path relies on (check() is called on every replayed
// record, so a batch that parses here but fails check() is fine — what must
// hold is that neither step trips ASan/UBSan).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include <unistd.h>

#include "db/write_batch_internal.h"
#include "strata/write_batch.h"
#include "util/env.h"
#include "wal/wal_reader.h"

namespace {

// Write the fuzz input to a per-process scratch file; the reader consumes
// real files through the Env, so the whole open path gets fuzzed too.
std::string scratch_path() {
    static const std::string path = "/tmp/strata-fuzz-wal-" + std::to_string(::getpid());
    return path;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace strata;
    Env* env = Env::default_env();
    {
        std::unique_ptr<WritableFile> f;
        if (!env->new_writable_file(scratch_path(), &f).ok()) {
            return 0;
        }
        f->append(Slice(reinterpret_cast<const char*>(data), size));
        f->close();
    }

    std::unique_ptr<WalReader> reader;
    // expected_uuid = 0 skips the UUID equality check: deeper coverage.
    if (!WalReader::open(env, scratch_path(), 0, &reader).ok()) {
        return 0;
    }
    std::string record;
    std::size_t total = 0;
    while (reader->read_record(&record)) {
        total += record.size();
        // Replay-path contract: check() must be safe on any CRC-valid bytes.
        WriteBatch batch;
        if (record.size() >= WriteBatchInternal::kHeader) {
            WriteBatchInternal::set_contents(&batch, Slice(record));
            (void)WriteBatchInternal::check(WriteBatchInternal::contents(&batch));
        }
        if (total > (64u << 20)) {
            __builtin_trap(); // decompression-bomb style blowup: impossible
        }
    }
    return 0;
}
