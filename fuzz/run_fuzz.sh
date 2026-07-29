#!/usr/bin/env bash
# usage: ./fuzz/run_fuzz.sh <wal|sstable|manifest> <seconds>
# Provisions a dedicated ASan/UBSan fuzz build, seeds the corpus from real
# files produced by the unit tests, and runs libFuzzer for the given budget.
set -euo pipefail

target="${1:?target: wal | sstable | manifest}"
seconds="${2:?seconds}"

cd "$(dirname "$0")/.."

# Apple clang ships no libFuzzer runtime; prefer brew LLVM when present.
CXX="clang++"
if [[ "$(uname)" == "Darwin" ]] && [[ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang++" ]]; then
    CXX="$(brew --prefix llvm)/bin/clang++"
    # Brew LLVM's libFuzzer runtime and the system libc++ disagree on
    # container annotations: libFuzzer's own corpus-directory listing trips a
    # container-overflow false positive before any target code runs. Keep the
    # rest of ASan live. (Linux CI runs with full checks.)
    export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_container_overflow=0"
fi

cmake -S . -B build/fuzz -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DSTRATA_SANITIZE=address,undefined \
    -DSTRATA_BUILD_FUZZERS=ON \
    -DSTRATA_BUILD_TESTS=OFF \
    -DSTRATA_BUILD_TOOLS=OFF >/dev/null
cmake --build build/fuzz --target "fuzz_${target}" >/dev/null

mkdir -p "fuzz/corpus/${target}"
exec "build/fuzz/fuzz/fuzz_${target}" "-max_total_time=${seconds}" -max_len=65536 \
    -rss_limit_mb=4096 "fuzz/corpus/${target}"
