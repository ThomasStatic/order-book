"""Recompute the v2.0.0 tables in docs/BENCHMARKS.md from the raw CSV files beside this script.

Usage: python summarize.py   (Python 3.8+, standard library only)

Warm-up runs are excluded. Percentiles are of per-run throughput (one value per run of
all its timed commands), by linear interpolation between closest ranks
(statistics.quantiles, method="inclusive"). They are not per-command latencies, which the
benchmark does not measure. The paired before/after comparisons are in paired-*/summary.md,
written by benchmarks/compare_ab.py.
"""

import csv
import statistics
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKLOADS = ["small-book", "deep-book", "matching-heavy", "cancellation-heavy", "mostly-resting",
             "quote-refresh", "pending-stops", "self-trade", "immediate", "matching-heavy-request",
             "matching-heavy-owned"]
KINDS = ["core", "replay"]


def measured(path):
    with open(path, newline="") as f:
        return [row for row in csv.DictReader(f) if row["warmup"] == "false"]


def ops(rows, workload, kind):
    return [float(r["ops_per_second"]) for r in rows if r["workload"] == workload and r["kind"] == kind]


def millions(x):
    return f"{x / 1e6:.2f}"


def book(rows, workload):
    row = next(r for r in rows if r["workload"] == workload and r["kind"] == "core")
    text = f'{int(row["final_active_orders"]):,} orders, {int(row["final_price_levels"]):,} levels'
    if int(row["final_pending_stops"]):
        text += f', {int(row["final_pending_stops"]):,} pending stops'
    return text


def main():
    sessions = [measured(HERE / f"standard-session{s}.csv") for s in (1, 2, 3)]
    pooled = [row for rows in sessions for row in rows]

    print("Standard size, pooled over 3 sessions")
    print()
    print("| Workload | Kind | Median Mops/s | ns/command at the median | p10 – p90 Mops/s | Min – max Mops/s | CV | Session medians |")
    print("|---|---|---:|---:|---|---|---:|---|")
    medians = {}
    for w in WORKLOADS:
        for k in KINDS:
            v = ops(pooled, w, k)
            q = statistics.quantiles(v, n=10, method="inclusive")
            med = statistics.median(v)
            medians[(w, k)] = med
            cv = statistics.stdev(v) / statistics.mean(v)
            per_session = " / ".join(millions(statistics.median(ops(rows, w, k))) for rows in sessions)
            print(f"| `{w}` | {k} | **{millions(med)}** | {1e9 / med:,.0f} | {millions(q[0])} – {millions(q[8])} | "
                  f"{millions(min(v))} – {millions(max(v))} | {cv:.1%} | {per_session} |")

    print()
    print("Final books at the standard size")
    print()
    for w in WORKLOADS:
        print(f"- `{w}`: {book(pooled, w)}")

    print()
    print("Replay-layer cost at the pooled medians (ns/command)")
    print()
    print("| Workload | core ns/command | replay ns/command | Difference |")
    print("|---|---:|---:|---:|")
    for w in WORKLOADS:
        core = 1e9 / medians[(w, "core")]
        replay = 1e9 / medians[(w, "replay")]
        print(f"| `{w}` | {core:,.0f} | {replay:,.0f} | {replay - core:,.0f} |")

    print()
    print("Extended size, one session")
    print()
    print("| Workload | Kind | Median Mops/s | ns/command at the median | Min – max Mops/s | Final book |")
    print("|---|---|---:|---:|---|---|")
    ext = measured(HERE / "extended.csv")
    for w in WORKLOADS:
        for k in KINDS:
            v = ops(ext, w, k)
            med = statistics.median(v)
            final = book(ext, w) if k == "core" else ""
            print(f"| `{w}` | {k} | {millions(med)} | {1e9 / med:,.0f} | {millions(min(v))} – {millions(max(v))} | {final} |")


if __name__ == "__main__":
    main()
