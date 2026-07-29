#!/usr/bin/env bash
# Runs the full YCSB comparison matrix and writes raw results to
# bench/results/. Method + fairness rules: docs/BENCHMARKS.md.
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH=build/release/bench/bench
RECORDS="${RECORDS:-1000000}"
OPS="${OPS:-1000000}"
SCAN_OPS=$((OPS / 5)) # scans touch up to 100 rows each
OUT=bench/results/ycsb.txt
BASE="${BENCH_DIR:-/tmp/strata-bench}"

mkdir -p "$BASE"
: > "$OUT"
run() {
    echo "+ $*" | tee -a "$OUT"
    "$@" 2>&1 | tee -a "$OUT"
    echo | tee -a "$OUT"
}

for engine in strata rocksdb; do
    for threads in 1 4; do
        dir="$BASE/$engine-t$threads"
        rm -rf "$dir"
        run $BENCH --engine $engine --workload load --dir "$dir" \
            --records "$RECORDS" --ops "$RECORDS" --threads "$threads" --sync 0
        for w in a b c; do
            run $BENCH --engine $engine --workload $w --dir "$dir" \
                --records "$RECORDS" --ops "$OPS" --threads "$threads" --sync 0
        done
        run $BENCH --engine $engine --workload e --dir "$dir" \
            --records "$RECORDS" --ops "$SCAN_OPS" --threads "$threads" --sync 0
    done

    # Synchronous-commit write path (group commit vs pipelined WAL).
    dir="$BASE/$engine-sync"
    rm -rf "$dir"
    run $BENCH --engine $engine --workload load --dir "$dir" \
        --records 50000 --ops 50000 --threads 4 --sync 1
done

echo "results in $OUT"
