#!/usr/bin/env python3
"""Recompute V2b's tables from what record_baseline.py wrote.

    python benchmarks/summarize_baseline.py --out DIR > DIR/summary.md

Each threaded kind's A/A floor; the gate, core against tag v2.0.0 on all eleven workloads at
standard and extended sizes, no more than 3% slower at the median ratio or flagged to explain,
and faster reported; every paired comparison, a threaded one near its kind's floor marked for
#33's re-check at 24 pairs; the zero-heap before-and-after; the median across sessions of every
other cell, with its lowest thread CPU share; the latency rows' merged p50 and p99 and whether
each can be quoted; and the steps that failed. Every figure is recomputed from the files, never
typed. record_baseline.py copies this into its results as summarize.py.
"""
import argparse
import csv
import glob
import os
import re
import statistics
import sys

GATE = 0.97  # no workload more than 3% slower
ELEVEN = 11  # the gate's workloads, each a core row at each size


def pairRows(path):
    """compare_ab.py's summary.md rows: (workload, kind, median, low, high, faster, pairs)."""
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, encoding="utf-8"):
        match = re.match(r"^\| (\S+) \| (\S+) \| ([0-9.]+) \| ([0-9.]+) - ([0-9.]+) \| (\d+)% \| (\d+) \|$", line.strip())
        if match:
            workload, kind, median, low, high, faster, pairs = match.groups()
            rows.append((workload, kind, float(median), float(low), float(high), int(faster), int(pairs)))
    return rows


def floors(out):
    """Each threaded kind's floor: the widest its A/A intervals reach from 1, over its workloads."""
    print("## A/A floors\n")
    print("| Kind | Workload | Median ratio | 95% CI | Floor |")
    print("|---|---|---:|---|---:|")
    byKind = {}
    for path in sorted(glob.glob(os.path.join(out, "pairs", "aa-*", "summary.md"))):
        name = os.path.basename(os.path.dirname(path))
        kind = name.split("-")[1]
        for workload, _, median, low, high, _, _ in pairRows(path):
            floor = max(abs(1 - low), abs(high - 1))
            byKind[kind] = max(byKind.get(kind, 0.0), floor)
            print(f"| {kind} | {workload} | {median:.3f} | {low:.3f} - {high:.3f} | ±{floor * 100:.1f}% |")
    print()
    for kind, floor in sorted(byKind.items()):
        print(f"- {kind}'s floor: ±{floor * 100:.1f}%")
    return byKind


def gate(out):
    print("\n## The gate: core against tag v2.0.0\n")
    print("| Size | Workload | Kind | Median ratio now/v2.0.0 | 95% CI | Verdict |")
    print("|---|---|---|---:|---|---|")
    failing = 0
    for size in ("standard", "extended"):
        rows = pairRows(os.path.join(out, "pairs", f"gate-{size}", "summary.md"))
        missing = ELEVEN - sum(1 for row in rows if row[1] == "core")
        if missing > 0:
            # A gate step that failed, or never ran, is no pass.
            failing += missing
            print(f"| {size} | {missing} of {ELEVEN} workloads | core | - | - | MISSING: the gate didn't run |")
        for workload, kind, median, low, high, _, _ in rows:
            if kind == "core":
                if median > 1:
                    verdict = "faster"
                elif median >= GATE:
                    verdict = "within 3%"
                else:
                    verdict = "MORE THAN 3% SLOWER: explain and accept, or fix"
                    failing += 1
            else:
                verdict = "reported, not gated"
            print(f"| {size} | {workload} | {kind} | {median:.3f} | {low:.3f} - {high:.3f} | {verdict} |")
    print(f"\n{failing} core row(s) outside the gate.")


def pairs(out, byKind):
    """Every pair; a threaded one whose interval reaches within its kind's floor of 1 is marked
    for #33's re-check at 24 pairs."""
    print("\n## Paired comparisons\n")
    print("| Pair | Workload | Kind | Median ratio exp/base | 95% CI | Exp faster in | Re-check |")
    print("|---|---|---|---:|---|---:|---|")
    marked = []
    for path in sorted(glob.glob(os.path.join(out, "pairs", "*", "summary.md"))):
        name = os.path.basename(os.path.dirname(path))
        if name.startswith(("aa-", "gate-")):
            continue
        for workload, kind, median, low, high, faster, _ in pairRows(path):
            floor = byKind.get(kind)
            near = floor is not None and not name.endswith("-24") and min(abs(1 - low), abs(high - 1), abs(1 - median)) <= floor
            if near:
                marked.append(name)
            print(f"| {name} | {workload} | {kind} | {median:.3f} | {low:.3f} - {high:.3f} | {faster}% | {'near the floor' if near else ''} |")
    if marked:
        print("\nRerun at 24 pairs: python benchmarks/record_baseline.py ... --only pairs " +
              " ".join(f"--recheck {name}" for name in sorted(set(marked))))


def reportRows(path):
    """Every row of a latency report: (title, quotable, p50, p99), merged values in ns, each with
    the report's marks (noise, unresolved) after it."""
    rows = []
    for block in open(path, encoding="utf-8").read().split("\n\n"):
        lines = block.strip().splitlines()
        if not lines or not lines[0].startswith("=="):
            continue
        values = {}
        quotable = ""
        for line in lines:
            if line.startswith("quotable:"):
                quotable = line[len("quotable:"):].strip()
            match = re.match(r"^(p50|p99)\s+(\d+)\s.*?((?:\s+(?:noise|unresolved))*)\s*$", line)
            if match:
                values[match.group(1)] = (int(match.group(2)), match.group(3).strip())
        p50, p99 = values.get("p50"), values.get("p99")
        rows.append((lines[0].strip("= "), quotable, p50, p99))
    return rows


def zeroHeap(out):
    """Command service time now against v2.0.0's library, workload by workload: each side's median
    over its sessions, and the ratio."""
    sides = {"before": {}, "now": {}}
    for path in glob.glob(os.path.join(out, "latency", "zero-heap-*", "report.txt")):
        side = os.path.basename(os.path.dirname(path)).rsplit("-", 1)[1]
        for title, _, p50, p99 in reportRows(path):
            sides[side].setdefault(title, []).append((p50[0] if p50 else None, p99[0] if p99 else None))
    if not sides["now"]:
        return
    print("\n## Zero-heap before and after: command service time, now against v2.0.0's library (ns)\n")
    print("| Row | p50 before | p50 now | Ratio | p99 before | p99 now | Ratio |")
    print("|---|---:|---:|---:|---:|---:|---:|")

    def median(values):
        values = [v for v in values if v is not None]
        return statistics.median(values) if values else None

    for title in sorted(sides["now"]):
        before, now = sides["before"].get(title, []), sides["now"][title]
        cells = []
        for index in (0, 1):
            b, n = median([v[index] for v in before]), median([v[index] for v in now])
            cells += ["-" if b is None else f"{b:.0f}", "-" if n is None else f"{n:.0f}", "-" if not b or n is None else f"{n / b:.3f}"]
        print(f"| {title} | " + " | ".join(cells) + " |")


def sessions(out):
    print("\n## Medians across sessions\n")
    cells = {}
    for path in glob.glob(os.path.join(out, "runs", "s*", "*.csv")):
        session = os.path.basename(os.path.dirname(path))
        cell = os.path.basename(path)[:-4]
        with open(path, newline="") as file:
            for row in csv.DictReader(file):
                if row["warmup"] in ("true", "1"):
                    continue
                key = (cell, row["kind"])
                entry = cells.setdefault(key, {"sessions": {}, "shares": [], "row": row})
                entry["sessions"].setdefault(session, []).append(float(row["ops_per_second"]))
                share = row.get("min_thread_cpu_share", "")
                if share:
                    entry["shares"].append(float(share))
    print("| Cell | Kind | N | M | Instruments | Spread | Placement | Layout | Durability | Sessions | Median ops/s | Session medians | Lowest thread CPU share |")
    print("|---|---|---:|---:|---:|---|---|---|---|---:|---:|---|---:|")
    for (cell, kind), entry in sorted(cells.items()):
        medians = [statistics.median(values) for _, values in sorted(entry["sessions"].items())]
        row = entry["row"]
        threads = row.get("gateway_threads") if row.get("gateway_threads", "0") != "0" else row.get("producers", "")
        share = f"{min(entry['shares']):.3f}" if entry["shares"] else "-"
        print(f"| {cell} | {kind} | {row.get('shards', '')} | {threads} | {row.get('instruments', '')} | {row.get('spread', '')} | "
              f"{row.get('placement', '')} | {row.get('layout', '')} | {row.get('durability', '')} | {len(medians)} | "
              f"{statistics.median(medians):,.0f} | {', '.join(f'{m:,.0f}' for m in medians)} | {share} |")


def latency(out):
    print("\n## Latency rows (ns)\n")
    print("| Row | Title | Quotable | p50 | p99 |")
    print("|---|---|---|---:|---:|")
    for path in sorted(glob.glob(os.path.join(out, "latency", "*", "report.txt"))):
        name = os.path.basename(os.path.dirname(path))
        if name.startswith("zero-heap-"):
            continue  # in its own table
        for title, quotable, p50, p99 in reportRows(path):
            cells = ["-" if value is None else f"{value[0]} {value[1]}".strip() for value in (p50, p99)]
            print(f"| {name} | {title} | {quotable} | {cells[0]} | {cells[1]} |")


def noise(out):
    """The latency harness's noise floor before and after each threaded session."""
    print("\n## Noise floors around the threaded sessions\n")
    for path in sorted(glob.glob(os.path.join(out, "noise", "*", "report.txt"))):
        name = os.path.basename(os.path.dirname(path))
        floors = [line.strip() for line in open(path, encoding="utf-8") if line.startswith("noise floor")][:2]
        for line in floors:
            print(f"- {name}: {line}")


def failures(out):
    log = os.path.join(out, "session.log")
    failed = [line.strip() for line in open(log, encoding="utf-8") if " FAILED " in line] if os.path.exists(log) else []
    print(f"\n## Steps that failed: {len(failed)}\n")
    for line in failed:
        print(f"- {line}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    out = os.path.abspath(parser.parse_args().out)
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")  # the floors' plus-minus, whatever the console's code page
    byKind = floors(out)
    gate(out)
    pairs(out, byKind)
    zeroHeap(out)
    sessions(out)
    latency(out)
    noise(out)
    failures(out)


if __name__ == "__main__":
    main()
