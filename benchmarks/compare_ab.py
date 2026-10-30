#!/usr/bin/env python3
"""Paired A/B comparison of two order_book_benchmarks binaries (docs/OPTIMIZATION-EXPERIMENTS.md).

Whole sessions of one binary followed by the other proved unusable here: a base-against-base
run of that design measured the identical binary 9% faster on matching-heavy in every
session, only because it ran second, and the large-book workloads drifted by 10-50% between
sessions. So this script interleaves finely. For each workload it makes `--pairs` pairs of
short invocations (`--repetitions` measured runs after one warm-up, both kinds), ordered
ABBA ABBA ... so that each binary runs first and second equally often, and adjacent runs
see nearly the same machine state. Each pair gives one ratio, experiment median / base
median; the result per workload and kind is the median of those ratios with a bootstrap 95%
confidence interval, and the share of pairs in which the experiment was faster.

    python benchmarks/compare_ab.py --base BASE.exe --exp EXP.exe --out DIR
        [--pairs 16] [--repetitions 5] [--size standard] [--kind both] [--workload NAME]...
        [--exp-workload NAME]

Writes every invocation's CSV to DIR, and DIR/summary.md.

With --exp-workload, the experiment's invocations run that workload in place of each
--workload, so one binary (passed as both --base and --exp) can compare two workloads under
the same interleaving: matching-heavy-owned against matching-heavy-request, for example.
"""
import argparse
import csv
import os
import random
import statistics
import subprocess
import sys

WORKLOADS = ["small-book", "deep-book", "matching-heavy", "cancellation-heavy", "mostly-resting"]


def run(binary, args, csv_path):
    command = [binary] + args + ["--csv", csv_path]
    result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode != 0:
        sys.exit(f"{' '.join(command)} exited with {result.returncode}: {result.stderr.decode(errors='replace')}")


def medians(csv_path):
    """{kind: median ops/s over the measured (non-warm-up) runs in this file}."""
    groups = {}
    with open(csv_path, newline="") as file:
        for row in csv.DictReader(file):
            if row["warmup"] in ("1", "true", "True"):
                continue
            groups.setdefault(row["kind"], []).append(float(row["ops_per_second"]))
    return {kind: statistics.median(values) for kind, values in groups.items()}


def bootstrap_median_interval(values, draws=4000, seed=1):
    generator = random.Random(seed)
    estimates = sorted(statistics.median(generator.choices(values, k=len(values))) for _ in range(draws))
    return estimates[int(0.025 * draws)], estimates[int(0.975 * draws) - 1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--exp", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--pairs", type=int, default=16)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--size", default="standard")
    parser.add_argument("--kind", default="both")
    parser.add_argument("--workload", action="append", default=[])
    parser.add_argument("--seed", default="42")
    parser.add_argument("--exp-workload")
    options = parser.parse_args()
    # Absolute, since Windows does not search the current directory for a relative path with slashes.
    binaries = {"base": os.path.abspath(options.base), "exp": os.path.abspath(options.exp)}
    os.makedirs(options.out, exist_ok=True)
    workloads = options.workload or WORKLOADS

    rows = []
    for workload in workloads:
        common = ["--size", options.size, "--kind", options.kind,
                  "--repetitions", str(options.repetitions), "--warmup", "1", "--seed", options.seed]
        args = {"base": ["--workload", workload] + common,
                "exp": ["--workload", options.exp_workload or workload] + common}
        ratios = {}
        for pair in range(options.pairs):
            order = ("base", "exp") if pair % 2 == 0 else ("exp", "base")
            result = {}
            for label in order:
                path = os.path.join(options.out, f"{workload}-{pair:02d}-{label}.csv")
                run(binaries[label], args[label], path)
                result[label] = medians(path)
            for kind in result["base"]:
                ratios.setdefault(kind, []).append(result["exp"][kind] / result["base"][kind])
        for kind, values in ratios.items():
            low, high = bootstrap_median_interval(values)
            faster = sum(1 for value in values if value > 1) / len(values)
            rows.append((workload, kind, statistics.median(values), low, high, faster, len(values)))
            print(f"{workload:20} {kind:7} median ratio {statistics.median(values):.3f}  95% CI {low:.3f}-{high:.3f}  "
                  f"exp faster in {faster:.0%} of {len(values)} pairs", flush=True)

    lines = ["| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |", "|---|---|---:|---|---:|---:|"]
    for workload, kind, median, low, high, faster, count in rows:
        lines.append(f"| {workload} | {kind} | {median:.3f} | {low:.3f} - {high:.3f} | {faster:.0%} | {count} |")
    with open(os.path.join(options.out, "summary.md"), "w", newline="\n") as summary:
        against = f"exp workload: {options.exp_workload}\n" if options.exp_workload else ""
        summary.write(f"base: {binaries['base']}\nexp: {binaries['exp']}\n{against}size {options.size}, {options.pairs} ABBA pairs per "
                      f"workload, {options.repetitions} measured runs per invocation, seed {options.seed}\n\n" + "\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
