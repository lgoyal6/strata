// libFuzzer target: arbitrary bytes as MANIFEST contents. parse_manifest is
// on the recovery path, so it must treat every byte as adversarial. Also
// asserts the round-trip invariant (taut style): anything that parses must
// re-encode and re-parse to an identical structure.

#include <cstdint>
#include <cstring>
#include <string>

#include "db/version.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace strata;
    ManifestData parsed;
    if (!parse_manifest(Slice(reinterpret_cast<const char*>(data), size), &parsed).ok()) {
        return 0;
    }
    // Round-trip: encode(parse(x)) must parse to the same structure.
    std::string encoded;
    encode_manifest(parsed, &encoded);
    ManifestData reparsed;
    if (!parse_manifest(Slice(encoded), &reparsed).ok()) {
        __builtin_trap();
    }
    if (reparsed.db_uuid != parsed.db_uuid ||
        reparsed.next_file_number != parsed.next_file_number ||
        reparsed.last_sequence != parsed.last_sequence ||
        reparsed.min_wal_number != parsed.min_wal_number) {
        __builtin_trap();
    }
    for (int level = 0; level < kNumLevels; ++level) {
        if (reparsed.files[level].size() != parsed.files[level].size()) {
            __builtin_trap();
        }
        for (std::size_t i = 0; i < parsed.files[level].size(); ++i) {
            if (reparsed.files[level][i].number != parsed.files[level][i].number ||
                reparsed.files[level][i].smallest != parsed.files[level][i].smallest ||
                reparsed.files[level][i].largest != parsed.files[level][i].largest) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
