#!/bin/bash
# Builds the browser demo: strata compiled to WebAssembly.
# Output: docs/demo/strata.js + strata.wasm (docs/ is the GitHub Pages root).
set -euo pipefail
cd "$(dirname "$0")/.."

SRCS=$(find src -name '*.cc' ! -name 'env.cc')

mkdir -p docs/demo
em++ $SRCS wasm/wasm_env.cc wasm/strata_wasm.cc \
  -Iinclude -Isrc \
  -std=c++20 -O2 \
  -pthread -sPTHREAD_POOL_SIZE=16 \
  -sMODULARIZE=1 -sEXPORT_NAME=StrataModule \
  -sENVIRONMENT=web,worker \
  -sINITIAL_MEMORY=268435456 -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
  -sSTACK_SIZE=1048576 \
  -o docs/demo/strata.js

ls -la docs/demo/
