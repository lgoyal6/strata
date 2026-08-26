// libFuzzer target: arbitrary bytes as an SSTable. Open, point-get, and full
// iteration must parse or reject - never crash, over-read, or loop forever.
// Every block is CRC-guarded, so most mutations die at open; to reach the
// block/index/filter parsers, mutated tables produced by the seed corpus
// (real tables) matter - see fuzz/run_fuzz.sh which seeds from unit-test
// artifacts.

#include <cstdint>
#include <memory>
#include <string>

#include <unistd.h>

#include "db/dbformat.h"
#include "strata/iterator.h"
#include "table/table_reader.h"
#include "util/env.h"

namespace {

std::string scratch_path() {
    static const std::string path = "/tmp/strata-fuzz-sst-" + std::to_string(::getpid());
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

    std::unique_ptr<RandomAccessFile> file;
    if (!env->new_random_access_file(scratch_path(), &file).ok()) {
        return 0;
    }
    Options options;
    InternalKeyComparator icmp;
    std::shared_ptr<TableReader> reader;
    if (!TableReader::open(options, icmp, std::move(file), 1, size, nullptr, nullptr, &reader)
             .ok()) {
        return 0;
    }

    // Point probes.
    for (const char* probe : {"", "a", "user-key-000042", "\xff\xff\xff\xff"}) {
        std::string ikey;
        append_internal_key(&ikey, probe, kMaxSequenceNumber, kValueTypeForSeek);
        std::string found_key, found_value;
        bool found = false;
        (void)reader->get(Slice(ikey), &found_key, &found_value, &found);
    }

    // Bounded full iteration (a malformed restart array must not loop).
    const std::unique_ptr<Iterator> it(reader->new_iterator());
    std::size_t steps = 0;
    for (it->seek_to_first(); it->valid(); it->next()) {
        (void)it->key();
        (void)it->value();
        if (++steps > 1u << 22) {
            __builtin_trap();
        }
    }
    return 0;
}
