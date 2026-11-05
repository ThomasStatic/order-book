#!/usr/bin/env bash
# V1 (#96): the three latency rows that can change a default, on the reference machine.
# Run from the repository root after a Release build: bash benchmarks/results/2026-11-05-latency-v1/run.sh
# Then: python benchmarks/results/2026-11-05-latency-v1/summarize.py
set -euo pipefail

here=benchmarks/results/2026-11-05-latency-v1
latency=./build/Release/order_book_latency_benchmarks
pinned=(--pin-core 6 --priority high --machine-record "$here/machine.txt")
log_dir="${TEMP:-/tmp}"

session() {
    local name=$1
    shift
    echo "$(date +%H:%M:%S) $name"
    "$latency" "$@" "${pinned[@]}" --out-dir "$here/$name" > /dev/null
}

# Spin window: hand-off at a 20 us, 100 us, 1 ms, and 20 ms gap, with each window, and unlimited
# twice at 20 us for its A/A. Commands per run keep each run's merged p50 printable: 9 runs of
# 250 at 20 ms leave 1,125 samples beyond it.
gaps=("50000 20000 20us" "10000 10000 100us" "1000 3000 1ms" "50 250 20ms")
for window in 0 20us 100us 1ms unlimited; do
    for gap in "${gaps[@]}"; do
        read -r rate commands label <<< "$gap"
        session "spin-$window-gap-$label" --interval hand-off --time response --rate "$rate" --commands "$commands" \
            --spin-window "$window" --workload small-book --matching-core 10
    done
done
session "spin-unlimited-gap-20us-again" --interval hand-off --time response --rate 50000 --commands 20000 \
    --spin-window unlimited --workload small-book --matching-core 10

# 10k/s durable wait: five 60-second runs at machine, on extended and on zero-filled files, beside
# the raw write-through loop at the same rate on the same files.
for fill in extended zero-filled; do
    session "durable-wait-10k-$fill" --interval durable-wait --time response --rate 10000 --commands 600000 --runs 5 \
        --warmup 0 --log-files "$fill" --workload small-book --writer-core 10 --log-dir "$log_dir"
    session "raw-write-10k-$fill" --interval raw-write --time response --rate 10000 --commands 600000 --runs 5 \
        --warmup 0 --log-files "$fill" --workload small-book --log-dir "$log_dir"
done

# Idle connections: the echo's service time beside 0 to 1,024 idle connections, on WSAPoll and on
# completion ports, at the default spin window and with an echo thread that always sleeps.
for sockets in poll completion-port; do
    for window in 100us 0; do
        for idle in 0 16 64 256 1024; do
            session "idle-$sockets-spin-$window-$idle" --interval idle-echo --sockets "$sockets" --idle-connections "$idle" \
                --spin-window "$window" --commands 50000 --workload small-book --server-core 10
        done
    done
done
# Unlimited's third A/A session at the 20 us gap, after every other row, so the floor spans the
# session's drift.
session "spin-unlimited-gap-20us-late" --interval hand-off --time response --rate 50000 --commands 20000 \
    --spin-window unlimited --workload small-book --matching-core 10
echo "$(date +%H:%M:%S) done"
