"""Recompute the tables in docs/BENCHMARKS.md from the raw CSV files beside this script.

Usage: python summarize.py   (Python 3.8+, standard library only)

Warm-up runs are excluded. Percentiles are of per-run throughput (one value per run of
all its timed commands), by linear interpolation between closest ranks
(statistics.quantiles, method="inclusive"). They are not per-command latencies, which the
benchmark does not measure.
"""

import csv
import statistics
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKLOADS = ["small-book", "deep-book", "matching-heavy", "cancellation-heavy", "mostly-resting"]
KINDS = ["core", "replay"]


def measured(path):
    with open(path, newline="") as f:
        return [row for row in csv.DictReader(f) if row["warmup"] == "false"]


def ops(rows, workload, kind):
    return [float(r["ops_per_second"]) for r in rows if r["workload"] == workload and r["kind"] == kind]


def millions(x):
    return f"{x / 1e6:.2f}"


def main():
    sessions = [measured(HERE / f"standard-session{s}.csv") for s in (1, 2, 3)]
    pooled = [row for rows in sessions for row in rows]

    print("Standard size, pooled over 3 sessions (Mops/s; ns/cmd at the median)")
    print("workload            kind     runs  p10   p25   median p75   p90   min   max   ns/cmd  cv")
    medians = {}
    for w in WORKLOADS:
        for k in KINDS:
            v = ops(pooled, w, k)
            q = statistics.quantiles(v, n=20, method="inclusive")  # 5% steps
            med = statistics.median(v)
            medians[(w, k)] = med
            cv = statistics.stdev(v) / statistics.mean(v)
            print(f"{w:<19} {k:<7} {len(v):>5}  {millions(q[1])}  {millions(q[4])}  {millions(med)}   "
                  f"{millions(q[14])}  {millions(q[17])}  {millions(min(v))}  {millions(max(v))}  "
                  f"{1e9 / med:>6.1f}  {cv:.1%}")

    print()
    print("Standard size, median per session (Mops/s), and the spread of those medians")
    for w in WORKLOADS:
        for k in KINDS:
            m = [statistics.median(ops(rows, w, k)) for rows in sessions]
            spread = (max(m) - min(m)) / min(m)
            print(f"{w:<19} {k:<7} " + "  ".join(millions(x) for x in m) + f"   +{spread:.0%}")

    print()
    print("Replay-layer cost at the pooled medians (ns/cmd): replay - core")
    for w in WORKLOADS:
        core = 1e9 / medians[(w, "core")]
        replay = 1e9 / medians[(w, "replay")]
        print(f"{w:<19} core {core:6.1f}  replay {replay:6.1f}  difference {replay - core:6.1f}")

    print()
    print("Extended size, one session (Mops/s; ns/cmd at the median)")
    ext = measured(HERE / "extended.csv")
    for w in WORKLOADS:
        for k in KINDS:
            v = ops(ext, w, k)
            med = statistics.median(v)
            print(f"{w:<19} {k:<7} {len(v):>2}  median {millions(med)}  min {millions(min(v))}  "
                  f"max {millions(max(v))}  ns/cmd {1e9 / med:6.1f}")


if __name__ == "__main__":
    main()
