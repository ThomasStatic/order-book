"""Recompute V1's (#96) tables from the sessions' report.txt files.

Usage: python benchmarks/results/2026-11-05-latency-v1/summarize.py

Each session directory holds one row of response or service time (and, for a timetable, the
driver's lateness after it). This reads each session's first row: its quotable line, its merged
percentiles in nanoseconds with the runs' median and range, and the record fields the rules need,
then prints the three tables and applies each row's stated rule (#33).
"""

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
WINDOWS = ["0", "20us", "100us", "1ms", "unlimited"]
GAPS = ["20us", "100us", "1ms", "20ms"]
IDLE = [0, 16, 64, 256, 1024]


def first_row(session):
    """The first row of a session's report: its stats by name and its record fields."""
    path = HERE / session / "report.txt"
    if not path.exists():
        return None
    text = path.read_text().split("\n\n")[0]
    row = {"quotable": "", "fields": {}, "stats": {}}
    for line in text.splitlines():
        if line.startswith("quotable:"):
            row["quotable"] = line[len("quotable:"):].strip()
        elif line.startswith("  ") and "=" in line:
            key, value = line.strip().split("=", 1)
            row["fields"][key] = value
        else:
            # A run's own column prints "-" when a run alone has too few samples beyond it.
            match = re.match(r"^(p50|p90|p99|p99\.9|max)\s+(\d+)\s+(\S+)\s+(\d+|-)\s+(\d+|-)\s+(\d+|-)", line)
            if match:
                name, merged, _, median, low, high = match.groups()
                row["stats"][name] = tuple(None if value == "-" else int(value) for value in (merged, median, low, high))
    return row


def stat(row, name, part=0):
    if row is None or name not in row["stats"]:
        return None
    return row["stats"][name][part]


def us(nanoseconds):
    return "-" if nanoseconds is None else f"{nanoseconds / 1000:.2f}"


def spin_window():
    print("## Spin window: hand-off response time p50 (us), and the matching thread's share of a core\n")
    print("| Window | " + " | ".join(f"{gap} gap" for gap in GAPS) + " |")
    print("|---|" + "---|" * len(GAPS))
    for window in WINDOWS:
        cells = []
        for gap in GAPS:
            row = first_row(f"spin-{window}-gap-{gap}")
            share = row["fields"].get("matching-cpu-share", "") if row else ""
            share = share.split("%")[0] + "%" if "%" in share else "-"
            cells.append(f"{us(stat(row, 'p50'))} ({share})")
        print(f"| `{window}` | " + " | ".join(cells) + " |")
    # Unlimited's A/A: three sessions at the 20 us gap, one in the sweep, one straight after it,
    # and one after every other row, so the floor spans the session's drift. Merged p50s only, as
    # the windows are judged by theirs.
    names = ["spin-unlimited-gap-20us", "spin-unlimited-gap-20us-again", "spin-unlimited-gap-20us-late"]
    sessions = [stat(first_row(name), "p50") for name in names]
    if any(p50 is None for p50 in sessions):
        print("\nAn A/A session is missing.")
        return
    low, high = min(sessions), max(sessions)
    print(f"\nA/A at the 20 us gap: unlimited's merged p50 over three sessions, {', '.join(us(p50) for p50 in sessions)} us:"
          f" {us(low)} to {us(high)} us.")
    chosen = None
    for window in WINDOWS:
        p50 = stat(first_row(f"spin-{window}-gap-20us"), "p50")
        # Within A/A: no slower than unlimited's own sessions were. The rule asks for a window that
        # loses nothing against never sleeping, so one faster than every unlimited session is
        # within it, not outside: read two-sided, the rule would refuse a window for beating
        # unlimited.
        within = p50 is not None and p50 <= high
        if window == "unlimited":
            within = True
        print(f"- `{window}`: p50 {us(p50)} us, {'within' if within else 'outside'} A/A of unlimited")
        if within and chosen is None:
            chosen = window
    print(f"\nRule: the default becomes the smallest window within A/A of unlimited at the 20 us gap: `{chosen}`.")


def durable_wait_set(suffix, title):
    print(f"\n## 10k/s durable wait at machine, {title}: response time (us), five 60-second runs\n")
    print("| Row | Files | p50 | p99 | p99.9 | max | runs' max, median and range |")
    print("|---|---|---|---|---|---|---|")
    rows = {}
    for kind, label in (("durable-wait", "durable wait"), ("raw-write", "raw write loop")):
        for fill in ("extended", "zero-filled"):
            row = first_row(f"{kind}-10k-{fill}{suffix}")
            rows[(kind, fill)] = row
            maximum = row["stats"].get("max") if row else None
            spread = "-" if maximum is None else f"{us(maximum[1])} ({us(maximum[2])} to {us(maximum[3])})"
            print(f"| {label} | {fill} | {us(stat(row, 'p50'))} | {us(stat(row, 'p99'))} | {us(stat(row, 'p99.9'))} |"
                  f" {us(stat(row, 'max'))} | {spread} |")
    # A stall is the tail standing far off the middle, at p99 (30,000 samples beyond it in
    # 3,000,000) or p99.9 (3,000). The engine stalls only where either ratio to p50 is more than
    # twice the raw loop's on the same files; within that, the raw loop stalls the same way, and
    # it is the drive or the OS.
    print()
    for fill in ("extended", "zero-filled"):
        engine, raw = rows[("durable-wait", fill)], rows[("raw-write", fill)]
        if any(stat(row, name) is None for row in (engine, raw) for name in ("p50", "p99", "p99.9")):
            print(f"- {fill}: a row is missing")
            continue
        ratios = {name: (stat(engine, name) / stat(engine, "p50"), stat(raw, name) / stat(raw, "p50")) for name in ("p99", "p99.9")}
        engine_only = any(mine > 2 * theirs for mine, theirs in ratios.values())
        verdict = ("only the engine stalls" if engine_only
                   else "the raw loop stalls the same way: the drive or the OS, documented as the environment")
        detail = "; ".join(f"{name} over p50 {mine:.1f} against {theirs:.1f}" for name, (mine, theirs) in ratios.items())
        print(f"- {fill}: durable wait against raw loop, {detail}: {verdict}.")


def durable_wait():
    # As run.sh ran them: the sender pinned, and the log writer pinned with --writer-core.
    durable_wait_set("", "pinned, the log writer with --writer-core")
    # As the engine runs its writer, never pinned: the set the rule is judged on.
    durable_wait_set("-unpinned", "unpinned, as the engine runs its log writer (the rule's set)")


def idle_connections():
    print("\n## Idle connections: echo service time p50 (us)\n")
    print("| Socket set, spin window | " + " | ".join(f"{idle} idle" for idle in IDLE) + " |")
    print("|---|" + "---|" * len(IDLE))
    for sockets in ("poll", "completion-port"):
        for window in ("100us", "0"):
            cells = [us(stat(first_row(f"idle-{sockets}-spin-{window}-{idle}"), "p50")) for idle in IDLE]
            print(f"| {sockets}, `{window}` | " + " | ".join(cells) + " |")
    # The rule is judged as the engine ships, at the default spin window; the echo thread that
    # always sleeps is shown beside it, and decides nothing.
    poll = stat(first_row("idle-poll-spin-100us-64"), "p50")
    port = stat(first_row("idle-completion-port-spin-100us-64"), "p50")
    if poll is not None and port is not None:
        ratio = poll / port
        verdict = "more than twice: recommend completion ports and file the follow-up" if ratio > 2 else "not more than twice"
        print(f"\nRule, at 64 idle connections and the default spin window: poll {us(poll)} us against completion ports"
              f" {us(port)} us, {ratio:.2f} times: {verdict}.")
    poll = stat(first_row("idle-poll-spin-0-64"), "p50")
    port = stat(first_row("idle-completion-port-spin-0-64"), "p50")
    if poll is not None and port is not None:
        print(f"With an echo thread that always sleeps: {poll / port:.2f} times.")


def main():
    spin_window()
    durable_wait()
    idle_connections()
    unquotable = []
    for path in sorted(HERE.glob("*/report.txt")):
        row = first_row(path.parent.name)
        if row and not row["quotable"].startswith("yes"):
            unquotable.append(f"{path.parent.name}: {row['quotable']}")
    if unquotable:
        print("\nUnquotable sessions:")
        for line in unquotable:
            print(f"- {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
