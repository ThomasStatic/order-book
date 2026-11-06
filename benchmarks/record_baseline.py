#!/usr/bin/env python3
"""V2b's recorded Windows set (#97; spec #58, #33, #32) in one unattended session.

    python benchmarks/record_baseline.py --out benchmarks/results/<date>-engine --machine-record FILE
        --base PATH/TO/v2.0.0/order_book_benchmarks.exe
        --before-latency PATH/TO/17a8013/order_book_latency_benchmarks.exe
        --before-component PATH/TO/17a8013/order_book_component_benchmarks.exe
        [--build build/Release] [--smoke] [--only SECTION]... [--recheck PAIR]... [--list]

In order: each threaded kind's A/A floor, the gate against tag v2.0.0, the zero-heap
before-and-after, the paired comparisons, three sessions of everything else, the component
costs and controls, and #32's latency set. Every threaded run is pinned at high priority by this
machine's layout below, but for one unpinned row per threaded kind. Each step writes under --out
and appends to --out/session.log, and a step that fails is logged and the session goes on.
--smoke runs every step at the smallest sizes the programs take, one session, two pairs, to
check the plumbing; its numbers mean nothing (the component groups run at their own sizes). A
threaded pair whose interval reaches its kind's A/A floor, which summarize_baseline.py marks, is
rerun at 24 pairs with --only pairs --recheck NAME, into pairs/NAME-24. The "before" binaries are v2.0.0's order_book_benchmarks and commit
17a8013's latency and component programs, which landed on a library unchanged from v2.0.0.
Then: python benchmarks/summarize_baseline.py --out DIR.
"""
import argparse
import datetime
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ELEVEN = ["small-book", "deep-book", "matching-heavy", "cancellation-heavy", "mostly-resting", "quote-refresh",
          "pending-stops", "self-trade", "immediate", "matching-heavy-request", "matching-heavy-owned"]
THREADED = ["matching-heavy-request", "quote-refresh", "immediate", "pending-stops", "deep-book"]
SECTIONS = ["aa", "gate", "zero-heap", "pairs", "sessions", "components", "latency", "allocation"]

# The reference machine: 6 physical cores, each with two logical processors, 2c and 2c+1. Physical
# core 0 comes last, since the OS favours it.
PHYSICAL = [1, 2, 3, 4, 5, 0]


def logical(core):
    return 2 * core


def sibling(core):
    return 2 * core + 1


def layout(kind, shards, threads, how):
    """--pin-cores and --layout for a cell: matching threads, then producers or gateway threads,
    then gateway's clients, then log writers on their matching threads' siblings where free."""
    if how == "unpinned":
        return ["--layout", "unpinned"]
    clients = threads if kind == "gateway" else 0
    second = 0 if kind == "shard" else threads
    needed = shards + second + clients
    if how == "siblings" and kind == "queue":
        # Each producer on its matching thread's sibling; log writers unpinned.
        cores = [logical(c) for c in PHYSICAL[:shards]] + [sibling(c) for c in PHYSICAL[:shards]]
        return ["--pin-cores", ",".join(map(str, cores)), "--layout", "producer-on-sibling"]
    if needed <= len(PHYSICAL):
        cores = [logical(c) for c in PHYSICAL[:needed]]
        writers = [sibling(c) for c in PHYSICAL[:shards]]
        name = "one-core-each"
    else:
        # Out of physical cores: the rest on siblings, in the same order.
        order = [logical(c) for c in PHYSICAL] + [sibling(c) for c in PHYSICAL]
        cores = order[:needed]
        writers = []
        name = "siblings"
    if kind != "shard":
        cores += writers
    return ["--pin-cores", ",".join(map(str, cores)), "--layout", name]


class Session:
    def __init__(self, options):
        self.options = options
        self.out = os.path.abspath(options.out)
        os.makedirs(self.out, exist_ok=True)
        build = os.path.abspath(options.build)
        self.bench = os.path.join(build, "order_book_benchmarks.exe" if os.name == "nt" else "order_book_benchmarks")
        self.latency = os.path.join(build, "order_book_latency_benchmarks.exe" if os.name == "nt" else "order_book_latency_benchmarks")
        self.component = os.path.join(build, "order_book_component_benchmarks.exe" if os.name == "nt" else "order_book_component_benchmarks")
        self.allocationTests = os.path.join(build, "matching_path_allocation_tests.exe" if os.name == "nt" else "matching_path_allocation_tests")
        self.smoke = options.smoke
        self.pairs = "2" if self.smoke else "16"
        self.sessions = 1 if self.smoke else 3
        self.failures = 0
        self.planned = []
        self.recheck = set(options.recheck or [])
        if not options.list:
            # The summary beside the results it reads, as #33 asks.
            shutil.copyfile(os.path.join(HERE, "summarize_baseline.py"), os.path.join(self.out, "summarize.py"))

    def log(self, line):
        if self.options.list:
            return
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        with open(os.path.join(self.out, "session.log"), "a", newline="\n") as file:
            file.write(f"{stamp} {line}\n")
        print(f"{stamp} {line}", flush=True)

    def size(self, name):
        """A size option, shrunk under --smoke."""
        if not self.smoke:
            return ["--size", name]
        return ["--commands", "40000" if name == "long" else "2000"]

    def step(self, name, command):
        self.planned.append(name)
        if self.options.list:
            print(name)
            return
        self.log(f"start {name}")
        with open(os.path.join(self.out, "session.log"), "a", newline="\n") as file:
            file.write("  " + " ".join(command) + "\n")
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        target = os.path.join(self.out, "logs", name.replace("/", "_") + ".txt")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as file:
            file.write(result.stdout)
        if result.returncode != 0:
            self.failures += 1
            self.log(f"FAILED {name}: exit {result.returncode}, see {os.path.relpath(target, self.out)}")
        else:
            self.log(f"done {name}")

    def benchmarks(self, name, args, binary=None):
        path = os.path.join(self.out, "runs", name + ".csv")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        repetitions = ["--repetitions", "1", "--warmup", "0"] if self.smoke else ["--repetitions", "5", "--warmup", "1"]
        self.step(name, [binary or self.bench] + args + repetitions + ["--csv", path, "--json", path[:-4] + ".json"])

    def paired(self, name, base, exp, args, size="extended"):
        pairs = self.pairs
        if self.recheck:
            if name not in self.recheck:
                return
            name, pairs = name + "-24", "24"
        out = os.path.join(self.out, "pairs", name)
        repetitions = "1" if self.smoke else "5"
        command = [sys.executable, os.path.join(HERE, "compare_ab.py"), "--base", base, "--exp", exp, "--out", out,
                   "--pairs", pairs, "--repetitions", repetitions]
        command += ["--size", "smoke" if self.smoke else size]
        self.step("pairs/" + name, command + args)

    def latencyRow(self, name, args, binary=None, size="extended"):
        out = os.path.join(self.out, "latency", name)
        if self.smoke:
            sizing = ["--commands", "500", "--runs", "2", "--warmup", "0", "--noise-seconds", "0"]
        elif size.isdigit():
            sizing = ["--commands", size]
        else:
            sizing = ["--size", size]
        record = ["--machine-record", self.options.machine_record] if self.options.machine_record else []
        self.step("latency/" + name, [binary or self.latency] + args + sizing + record + ["--out-dir", out])

    def wanted(self, section):
        return not self.options.only or section in self.options.only

    def noiseFloor(self, name):
        """The latency harness's noise floor, before or after a threaded session: a tiny pinned
        command row whose report carries the floor's gaps."""
        sizing = ["--commands", "1000", "--runs", "1", "--warmup", "0"] + (["--noise-seconds", "1"] if self.smoke else [])
        record = ["--machine-record", self.options.machine_record] if self.options.machine_record else []
        self.step(name, [self.latency, "--workload", "small-book", "--pin-core", "6", "--priority", "high"] + sizing + record +
                  ["--out-dir", os.path.join(self.out, "noise", name.replace("/", "-"))])


def threaded(kind, shards, threads, how, workload, extra=()):
    instruments = str(8 * shards)
    args = ["--kind", kind, "--workload", workload, "--shards", str(shards), "--instruments", instruments, "--priority", "high"]
    # M = 1 is the default, so a cell at M = 1 can pair with another kind sharing these arguments.
    if kind == "queue" and threads != 1:
        args += ["--producers", str(threads)]
    elif kind == "gateway" and threads != 1:
        args += ["--gateway-threads", str(threads)]
    return args + layout(kind, shards, threads, how) + list(extra)


def expArgs(args):
    """--exp-args for compare_ab.py, in the = form, since its value starts with a dash."""
    return "--exp-args=" + " ".join(args)


def scalingCells():
    """(kind, N, M, layout) for #33's uniform scaling cells."""
    cells = [("shard", n, 1, "one-core") for n in (1, 2, 3, 4, 6)]
    cells += [("queue", n, n, "separate") for n in (1, 2, 3)]
    cells += [("queue", n, n, "siblings") for n in (2, 3, 4, 6)]
    cells += [("queue", n, 1, "separate") for n in (2, 3)]
    cells += [("gateway", n, m, "separate") for n, m in ((1, 1), (2, 1), (2, 2), (4, 1), (4, 2))]
    return cells


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    parser.add_argument("--build", default=os.path.join("build", "Release"))
    parser.add_argument("--base", help="tag v2.0.0's order_book_benchmarks, for the gate")
    parser.add_argument("--before-latency", help="commit 17a8013's order_book_latency_benchmarks, for the zero-heap before-and-after")
    parser.add_argument("--before-component", help="commit 17a8013's order_book_component_benchmarks, for growTo against v2.0.0")
    parser.add_argument("--machine-record")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--only", action="append", choices=SECTIONS)
    parser.add_argument("--recheck", action="append", help="rerun this pair at 24 pairs, into pairs/NAME-24; repeatable")
    parser.add_argument("--list", action="store_true", help="print the steps and exit")
    options = parser.parse_args()
    s = Session(options)
    cur = s.bench
    base = options.base or (cur if options.smoke else None)
    if s.wanted("gate") and base is None and not options.list:
        sys.exit("--base is needed for the gate, except under --smoke")
    pinned = ["--priority", "high"]

    if s.wanted("aa"):
        # Each threaded kind's floor first, at N = 2, before any threaded difference is read.
        for kind, threads, how in (("shard", 1, "one-core"), ("queue", 2, "separate"), ("gateway", 1, "separate")):
            for workload in ("quote-refresh", "deep-book"):
                s.paired(f"aa-{kind}-{workload}", cur, cur, threaded(kind, 2, threads, how, workload))
        for workload in ("quote-refresh", "deep-book"):
            s.paired(f"aa-engine-{workload}", cur, cur,
                     ["--kind", "engine", "--workload", workload, "--shards", "2", "--instruments", "16", "--durability", "process"])

    if s.wanted("gate"):
        for size in ("standard", "extended"):
            s.paired(f"gate-{size}", base, cur, ["--kind", "both"] + sum((["--workload", w] for w in ELEVEN), []), size=size)

    if s.wanted("zero-heap"):
        before = options.before_latency or (s.latency if options.smoke else None)
        if before is None:
            s.log("skipped zero-heap: no --before-latency")
        else:
            # ABBA sessions of command service time, the zero-heap engine against v2.0.0's library.
            for index, (label, binary) in enumerate((("before", before), ("now", s.latency), ("now", s.latency), ("before", before))):
                s.latencyRow(f"zero-heap-{index}-{label}", ["--pin-core", "6"] + pinned, binary=binary)

    if s.wanted("pairs"):
        for workload in THREADED:
            # Each adjacent pair of kinds at N = 1, a shard's eight instruments from shard on.
            # core can't pin itself, so only the shard side is placed.
            s.paired(f"core-shard-{workload}", cur, cur, ["--kind", "core", "--exp-kind", "shard", "--workload", workload,
                     expArgs(["--priority", "high"] + layout("shard", 1, 1, "one-core"))])
            for low, high in (("shard", "queue"), ("queue", "gateway")):
                # The same books on each side; each kind's own cores, the later --pin-cores replacing.
                args = threaded(low, 1, 1, "one-core", workload)
                s.paired(f"{low}-{high}-{workload}", cur, cur, args[2:] + ["--kind", low, "--exp-kind", high,
                         expArgs(layout(high, 1, 1, "one-core"))])
            # Both unpinned: the engine kind runs the shipped engine, which this program can't place.
            s.paired(f"gateway-engine-{workload}", cur, cur,
                     ["--kind", "gateway", "--exp-kind", "engine", "--workload", workload, "--instruments", "8", "--durability", "process"])
            s.paired(f"none-process-{workload}", cur, cur, threaded("queue", 1, 1, "separate", workload)[2:] + ["--kind", "queue",
                     expArgs(["--durability", "process"])])
            s.paired(f"process-machine-{workload}", cur, cur, threaded("queue", 1, 1, "separate", workload)[2:] + ["--kind", "queue",
                     "--durability", "process", expArgs(["--durability", "machine"])])
            for n in (2, 3, 4, 6):
                one = threaded("shard", 1, 1, "one-core", workload)
                s.paired(f"shard-{n}-against-1-{workload}", cur, cur, one[2:] + ["--kind", "shard",
                         expArgs(["--shards", str(n), "--instruments", str(8 * n)] + layout("shard", n, 1, "one-core"))])
        for workload in ("quote-refresh", "pending-stops", "immediate"):
            args = threaded("queue", 1, 1, "separate", workload)[2:] + ["--kind", "queue", "--durability", "process"]
            s.paired(f"worker-long-{workload}", cur, cur, args + [expArgs(["--roll-worker", "on"])], size="long")

    if s.wanted("sessions"):
        for session in range(1, s.sessions + 1):
            tag = f"s{session}"
            s.noiseFloor(f"{tag}/noise-before")
            for workload in ELEVEN:
                s.benchmarks(f"{tag}/both-standard-{workload}", ["--kind", "both", "--workload", workload] + s.size("standard"))
            if session == 1:
                for workload in ELEVEN:
                    s.benchmarks(f"{tag}/both-extended-{workload}", ["--kind", "both", "--workload", workload] + s.size("extended"))
            for kind, n, m, how in scalingCells():
                for workload in THREADED:
                    s.benchmarks(f"{tag}/{kind}-{n}-{m}-{how}-{workload}", threaded(kind, n, m, how, workload) + s.size("extended"))
            for kind, n, m in (("shard", 2, 1), ("queue", 2, 2), ("gateway", 2, 1)):
                for workload in THREADED:
                    s.benchmarks(f"{tag}/{kind}-{n}-{m}-unpinned-{workload}", threaded(kind, n, m, "unpinned", workload) + s.size("extended"))
            for kind, n, m in (("shard", 4, 1), ("queue", 3, 3), ("gateway", 4, 2)):
                for placement in ("hash", "hot-alone"):
                    args = threaded(kind, n, m, "separate", "quote-refresh", ["--spread", "skewed", "--placement", placement])
                    args[args.index("--instruments") + 1] = "32"
                    s.benchmarks(f"{tag}/skewed-{kind}-{n}-{m}-{placement}", args + s.size("extended"))
            for kind, n, m in (("queue", 1, 1), ("queue", 2, 2), ("gateway", 1, 1), ("gateway", 2, 1)):
                for level in ("process", "machine"):
                    for fill in ("extended", "zero-filled"):
                        for workload in ("quote-refresh", "deep-book"):
                            s.benchmarks(f"{tag}/durable-{kind}-{n}-{m}-{level}-{fill}-{workload}",
                                         threaded(kind, n, m, "separate", workload, ["--durability", level, "--log-files", fill]) +
                                         s.size("extended"))
            for n in (1, 2):
                for workload in ("quote-refresh", "pending-stops", "immediate"):
                    for worker in ("off", "on"):
                        s.benchmarks(f"{tag}/long-{n}-worker-{worker}-{workload}",
                                     threaded("queue", n, n, "separate", workload, ["--durability", "process", "--roll-worker", worker]) +
                                     s.size("long"))
            for kind in ("shard", "queue"):
                for books in (1, 8, 64, 512):
                    for workload in ("quote-refresh", "matching-heavy-request"):
                        args = threaded(kind, 1, 1, "separate", workload)
                        args[args.index("--instruments") + 1] = str(books)
                        s.benchmarks(f"{tag}/books-{kind}-{books}-{workload}", args + s.size("extended"))
            for window in (1, 16, 256, 4096):
                args = threaded("gateway", 1, 1, "separate", "quote-refresh", ["--window", str(window)])
                args[args.index("--instruments") + 1] = "1"
                s.benchmarks(f"{tag}/window-{window}", args + s.size("extended"))
            for connections in (1, 4, 16, 64):
                args = threaded("gateway", 1, 1, "separate", "quote-refresh")
                args[args.index("--instruments") + 1] = str(connections)
                s.benchmarks(f"{tag}/connections-{connections}", args + s.size("extended"))
            for budget in ("64", "unlimited"):
                args = threaded("gateway", 1, 1, "separate", "quote-refresh", ["--frame-budget", budget])
                args[args.index("--instruments") + 1] = "16"
                s.benchmarks(f"{tag}/frame-budget-{budget}", args + s.size("extended"))
            for kind, n, m in (("shard", 2, 1), ("queue", 2, 2), ("gateway", 2, 1)):
                s.benchmarks(f"{tag}/mixed-{kind}-{n}-{m}", threaded(kind, n, m, "separate", "mixed") + s.size("extended"))
            for n in (1, 2):
                for workload in THREADED:
                    s.benchmarks(f"{tag}/engine-{n}-1-{workload}", ["--kind", "engine", "--workload", workload, "--shards", str(n),
                                 "--instruments", str(8 * n), "--durability", "process"] + s.size("extended"))
            s.noiseFloor(f"{tag}/noise-after")

    if s.wanted("components"):
        out = os.path.join(s.out, "components")
        os.makedirs(out, exist_ok=True)
        small = ["--size", "smoke"] if s.smoke else []
        for group in ("capacity", "log", "entry", "wire", "images", "durability"):
            s.step(f"components/{group}", [s.component, "--group", group, "--csv", os.path.join(out, f"{group}.csv")] + small)
        before = options.before_component or (s.component if options.smoke else None)
        if before:
            s.step("components/capacity-before", [before, "--group", "capacity", "--csv", os.path.join(out, "capacity-before.csv")])
        # The controls on each layout's own cores: a shard count's, a producer on each matching
        # thread's sibling, and gateway's once it runs out of physical cores.
        controls = [(f"one-core-{n}", [logical(c) for c in PHYSICAL[:n]]) for n in (1, 2, 3, 4, 6)]
        controls += [(f"siblings-{n}", [logical(c) for c in PHYSICAL[:n]] + [sibling(c) for c in PHYSICAL[:n]]) for n in (2, 3, 4, 6)]
        gateway42 = layout("gateway", 4, 2, "separate")[1]
        controls.append(("gateway-4-2", [int(c) for c in gateway42.split(",")]))
        for name, cores in controls:
            s.step(f"components/controls-{name}", [s.component, "--group", "controls", "--threads", str(len(cores)), "--pin-cores",
                                                   ",".join(map(str, cores)), "--priority", "high",
                                                   "--csv", os.path.join(out, f"controls-{name}.csv")])

    if s.wanted("latency"):
        sender = ["--pin-core", "6"] + pinned
        for workload in ELEVEN:
            s.latencyRow(f"command-{workload}", ["--workload", workload] + sender)
        for workload in ("matching-heavy-request", "pending-stops"):
            s.latencyRow(f"command-ladder-{workload}", ["--workload", workload, "--time", "response"] + sender)
        handoff = ["--interval", "hand-off", "--workload", "matching-heavy-request", "--matching-core", "10"] + sender
        s.latencyRow("hand-off", handoff)
        s.latencyRow("hand-off-ladder", handoff + ["--time", "response"])
        # 50 a second, a 20 ms gap: each command wakes a sleeping matching thread. Smoke size, 10k
        # commands a run, already takes 200 s a run.
        s.latencyRow("hand-off-idle-wake", handoff + ["--time", "response", "--rate", "50"], size="2000")
        for level in ("process", "machine"):
            durable = ["--interval", "durable-wait", "--workload", "matching-heavy-request", "--durability", level]
            # Standard size, not #32's extended: a durable wait's ladder at machine runs at a tenth of
            # some 20k/s, and 1M commands a run would take hours a row.
            # The pair differs by the worker alone: the same pins and files of 10k records, so
            # files close, and in one a worker rolls each from an empty book's checkpoint.
            ladder = durable + ["--time", "response", "--writer-core", "10", "--log-file-records", "10000"] + sender
            s.latencyRow(f"durable-wait-{level}-ladder", ladder, size="standard")
            s.latencyRow(f"durable-wait-{level}-ladder-rolling", ladder + ["--roll-worker", "on"], size="standard")
        engine = os.path.join(os.path.dirname(s.bench), "order_book_engine.exe" if os.name == "nt" else "order_book_engine")
        trip = ["--interval", "round-trip", "--engine", engine, "--workload", "matching-heavy-request", "--matching-core", "10",
                "--gateway-core", "8", "--receiver-core", "4"] + sender
        if s.smoke:
            trip += ["--discard-seconds", "0"]  # 500 commands would fall inside the default 2 s
        for name, extra in (("process", ["--durability", "process"]), ("machine", ["--durability", "machine"]),
                            ("machine-63-idle", ["--durability", "machine", "--idle-connections", "63"])):
            s.latencyRow(f"round-trip-{name}", trip + extra)
            # Standard size too: #32 put a round trip's ladder at 21 minutes a row at extended.
            s.latencyRow(f"round-trip-{name}-ladder", trip + extra + ["--time", "response"], size="standard")
        s.latencyRow("round-trip-idle-wake", trip + ["--durability", "machine", "--time", "response", "--rate", "50"], size="2000")

    if s.wanted("allocation"):
        s.step("allocation/matching_path_allocation_tests", [s.allocationTests])

    if options.list:
        print(f"{len(s.planned)} steps")
        return 0
    s.log(f"session finished: {s.failures} step(s) failed")
    return 1 if s.failures else 0


if __name__ == "__main__":
    sys.exit(main())
