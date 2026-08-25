#!/usr/bin/env bash
# Builds the crash harness that docs/demo.tape records against.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build/demo -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/demo -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" --target crash_test >/dev/null
rm -rf /tmp/strata-demo
echo "ready: $(pwd)/build/demo/tools/crash_test"
echo "now run: vhs docs/demo.tape"
