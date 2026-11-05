#!/usr/bin/env bash
# V1 (#96), the 10k/s durable-wait row again, with every thread unpinned: the engine never pins
# its log writer, and pinned with --writer-core the writer alone stalled for milliseconds where
# the raw loop on the same files did not (see docs/LATENCY.md). Run after run.sh, from the
# repository root: bash benchmarks/results/2026-11-05-latency-v1/run_unpinned.sh
set -euo pipefail

here=benchmarks/results/2026-11-05-latency-v1
latency=./build/Release/order_book_latency_benchmarks
log_dir="${TEMP:-/tmp}"

for fill in extended zero-filled; do
    for interval in durable-wait raw-write; do
        name="$interval-10k-$fill-unpinned"
        echo "$(date +%H:%M:%S) $name"
        "$latency" --interval "$interval" --time response --rate 10000 --commands 600000 --runs 5 --warmup 0 \
            --log-files "$fill" --workload small-book --priority high --log-dir "$log_dir" \
            --machine-record "$here/machine.txt" --out-dir "$here/$name" > /dev/null
    done
done
echo "$(date +%H:%M:%S) done"
