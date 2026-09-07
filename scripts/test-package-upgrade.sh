#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/old"
git -C "$root" archive v0.1.0 | tar -x -C "$tmp/old"

install_package() {
  source_dir="$1"
  build_dir="$2"
  cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DSTRATA_INSTALL=ON -DSTRATA_BUILD_TESTS=OFF -DSTRATA_BUILD_TOOLS=OFF
  cmake --build "$build_dir" --target strata
  cmake --install "$build_dir" --prefix "$tmp/prefix"
}

run_consumer() {
  expected="$1"
  build_dir="$2"
  cmake -S "$root/tests/compat/consumer" -B "$build_dir" \
    -DCMAKE_PREFIX_PATH="$tmp/prefix" -DEXPECTED_STRATA_VERSION="$expected"
  cmake --build "$build_dir"
  "$build_dir/strata_compat_consumer" "$tmp/db-$expected"
}

install_package "$tmp/old" "$tmp/build-old"
run_consumer 0.1.0 "$tmp/consumer-old"

install_package "$root" "$tmp/build-current"
run_consumer 0.1.1 "$tmp/consumer-current"

if cmake -S "$root/tests/compat/incompatible" -B "$tmp/incompatible" \
  -DCMAKE_PREFIX_PATH="$tmp/prefix" >/dev/null 2>&1; then
  echo "incompatible 0.2 consumer unexpectedly accepted the 0.1 package" >&2
  exit 1
fi

echo "strata package upgrade v0.1.0 to v0.1.1 passed"
