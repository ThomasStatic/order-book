"""Draw the data-driven figures in this directory from the files they illustrate.

Usage: python docs/images/make_figures.py   (Python 3.8+, standard library only)

- benchmark-baseline.svg: every measured standard-size run of the recorded baseline, from
  benchmarks/results/2026-10-25-baseline/standard-session{1,2,3}.csv (warm-ups excluded).
- optimization.svg: the before-and-after tables of docs/OPTIMIZATION.md (ns per command and
  the heap's share of core time), and allocations per timed command from the two
  allocations.txt profiles in benchmarks/results/.
- replay-terminal.svg: two CLI runs, drawn from tests/replay/expected/, which CTest compares
  byte for byte with the CLI's real output, so the picture cannot drift from the program.

The architecture, lifecycle, walkthrough, and ladder diagrams are edited as SVG directly. Rerun this
script after changing any file it reads.
"""

import csv
import re
import statistics
from html import escape
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORKLOADS = ["small-book", "deep-book", "matching-heavy", "cancellation-heavy", "mostly-resting"]

# Light values first; the dark block restates them for a dark viewer. Text never takes a
# series color: marks carry identity, and labels stay in the ink tones.
STYLE = """
  .bg{fill:#fcfcfb} .frame{fill:none;stroke:rgba(11,11,11,.10)}
  text{font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;fill:#0b0b0b}
  .mono{font-family:ui-monospace,"Cascadia Mono",Consolas,Menlo,monospace}
  .t2{fill:#52514e} .t3{fill:#898781}
  .grid{stroke:#e1e0d9;stroke-width:1} .axis{stroke:#c3c2b7;stroke-width:1}
  .core{fill:#2a78d6} .replay{fill:#eb6834}
  .before{fill:#fcfcfb;stroke:#898781;stroke-width:2} .after{fill:#2a78d6;stroke:#fcfcfb;stroke-width:2}
  .link{stroke:#c3c2b7;stroke-width:2;stroke-linecap:round}
  .median{stroke:#0b0b0b;stroke-width:2;stroke-linecap:round}
  @media (prefers-color-scheme: dark){
    .bg{fill:#1a1a19} .frame{stroke:rgba(255,255,255,.10)}
    text{fill:#f0efec} .t2{fill:#c3c2b7} .t3{fill:#898781}
    .grid{stroke:#2c2c2a} .axis{stroke:#383835}
    .core{fill:#3987e5} .replay{fill:#d95926}
    .before{fill:#1a1a19;stroke:#898781} .after{fill:#3987e5;stroke:#1a1a19}
    .link{stroke:#383835} .median{stroke:#f0efec}
  }
"""


def svg(width, height, title, desc, body, extra_style=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">\n'
            f'<title id="title">{escape(title)}</title>\n<desc id="desc">{escape(desc)}</desc>\n'
            f'<style>{STYLE}{extra_style}</style>\n'
            f'<rect class="bg" width="{width}" height="{height}" rx="12"/>\n'
            f'<rect class="frame" x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="12"/>\n'
            + "\n".join(body) + "\n</svg>\n")


def text(x, y, s, cls="", size=13, anchor="start", weight=None):
    w = f' font-weight="{weight}"' if weight else ""
    c = f' class="{cls}"' if cls else ""
    return f'<text x="{x:.1f}" y="{y:.1f}"{c} font-size="{size}" text-anchor="{anchor}"{w}>{escape(s)}</text>'


def write(name, content):
    (HERE / name).write_text(content, encoding="utf-8", newline="\n")
    print(f"wrote {name}")


# ---- benchmark-baseline.svg ---------------------------------------------------------------

def benchmark_baseline():
    rows = []
    for s in (1, 2, 3):
        with open(ROOT / f"benchmarks/results/2026-10-25-baseline/standard-session{s}.csv", newline="") as f:
            rows += [r for r in csv.DictReader(f) if r["warmup"] == "false"]

    width, x0, x1, top, band = 960, 220, 680, 140, 66
    xmax = 7.0
    scale = (x1 - x0) / xmax
    bottom = top + band * len(WORKLOADS)
    body = [
        text(24, 38, "Benchmark baseline: every run, before optimization", size=18, weight=600),
        text(24, 61, "Throughput of each run of 100,000 timed commands, 90 runs per row (3 sessions × 30). "
                     "Each dot is one run; the bar is the median.", cls="t2"),
        text(24, 80, "Recorded 2026-10-25 on one machine (Ryzen 5 5600G, MSVC Release). Higher is faster.",
             cls="t2"),
        '<circle class="core" cx="30" cy="105" r="5"/>', text(42, 109, "core: OrderBook called directly"),
        '<circle class="replay" cx="262" cy="105" r="5"/>',
        text(274, 109, "replay: the same commands as in-memory CSV through the replay engine"),
        text(x1 + 30, top - 10, "median", cls="t3", size=12), text(x1 + 120, top - 10, "CV", cls="t3", size=12),
        text(x1 + 170, top - 10, "min–max", cls="t3", size=12),
    ]
    for tick in range(0, 8):
        x = x0 + tick * scale
        body.append(f'<line class="grid" x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{bottom}"/>')
        body.append(text(x, bottom + 18, str(tick), cls="t3", size=12, anchor="middle"))
    body.append(text((x0 + x1) / 2, bottom + 38, "millions of commands per second", cls="t3", size=12,
                     anchor="middle"))

    for i, w in enumerate(WORKLOADS):
        y = top + i * band
        last = next(r for r in rows if r["workload"] == w and r["kind"] == "core")
        body.append(text(24, y + 26, w, cls="mono", size=13, weight=600))
        body.append(text(24, y + 44, f'ends with {int(last["final_active_orders"]):,} orders', cls="t3", size=12))
        body.append(text(24, y + 59, f'on {int(last["final_price_levels"]):,} levels', cls="t3", size=12))
        if i:
            body.append(f'<line class="axis" x1="24" y1="{y}" x2="{width - 24}" y2="{y}"/>')
        for kind, cy in (("core", y + 22), ("replay", y + 46)):
            v = [float(r["ops_per_second"]) / 1e6 for r in rows if r["workload"] == w and r["kind"] == kind]
            for j, value in enumerate(v):
                jitter = ((j * 37) % 13 - 6) * 0.9  # deterministic, so reruns give the same file
                body.append(f'<circle class="{kind}" cx="{x0 + value * scale:.1f}" cy="{cy + jitter:.1f}" r="3.2" '
                            f'fill-opacity="0.45"><title>{w} {kind}, run {j + 1}: {value:.2f} Mops/s</title></circle>')
            med = statistics.median(v)
            cv = statistics.stdev(v) / statistics.mean(v)
            mx = x0 + med * scale
            body.append(f'<line class="median" x1="{mx:.1f}" y1="{cy - 9}" x2="{mx:.1f}" y2="{cy + 9}">'
                        f'<title>{w} {kind} median: {med:.2f} Mops/s</title></line>')
            body.append(f'<circle class="{kind}" cx="{x1 + 34}" cy="{cy}" r="4"/>')
            body.append(text(x1 + 44, cy + 4, f"{med:.2f}", size=13, weight=600))
            body.append(text(x1 + 80, cy + 4, kind, cls="t3", size=12))
            body.append(text(x1 + 120, cy + 4, f"{cv:.1%}", cls="t2", size=13))
            body.append(text(x1 + 170, cy + 4, f"{min(v):.2f}–{max(v):.2f}", cls="t2", size=13))

    y = bottom + 74
    body += [
        text(24, y, "Replay sits near 1 Mops/s on every workload: CSV parsing and event collection cost about "
                    "600 ns per command, 1.5–4× the engine's own cost.", cls="t2"),
        text(24, y + 20, "The large-book core rows scatter widely (CV 11–22%), so later changes were judged by "
                         "interleaved A/B pairs, not against these runs.", cls="t2"),
        text(24, y + 44, "Source: benchmarks/results/2026-10-25-baseline/ · tables in docs/BENCHMARKS.md",
             cls="t3", size=12),
    ]
    write("benchmark-baseline.svg", svg(width, y + 66, "Benchmark baseline",
                                        "Strip plot of per-run throughput for five workloads, core and replay.",
                                        body))


# ---- optimization.svg ---------------------------------------------------------------------

def optimization():
    report = (ROOT / "docs/OPTIMIZATION.md").read_text(encoding="utf-8")
    ns = {}
    for w, kind, before, after, ratio in re.findall(
            r"^\| `([a-z-]+)` \| (core|replay) \| ([\d,]+) \| ([\d,]+) \| \*\*([\d.]+)\*\*", report, re.M):
        ns[(w, kind)] = (int(before.replace(",", "")), int(after.replace(",", "")), float(ratio))
    heap_row = re.search(r"^\| heap \(`ntdll`, `ucrtbase`\) \|(.*)\|$", report, re.M).group(1)
    heap = dict(zip(WORKLOADS, [tuple(int(n) for n in cell.split("→")) for cell in heap_row.split("|")]))

    def allocations(path):
        found = {}
        for w, per in re.findall(r"^## ([a-z-]+), core\n\n.*\n  timed: [^(]*\(([\d.]+) per command\)",
                                 (ROOT / path).read_text(encoding="utf-8"), re.M):
            found[w] = float(per)
        return found
    alloc_before = allocations("benchmarks/results/2026-10-26-profile/allocations.txt")
    alloc_after = allocations("benchmarks/results/2026-10-26-final-profile/allocations.txt")
    assert len(ns) == 10 and len(heap) == 5 and len(alloc_before) == len(alloc_after) == 5

    width, label_w, row = 960, 196, 30
    facets = [(label_w, 548), (600, 920)]

    def dumbbell_panel(y0, heading, note, specs):
        """specs: per facet, (title, axis max, ticks, unit, {workload: (before, after, label)})."""
        out = [text(24, y0, heading, size=15, weight=600), text(24, y0 + 20, note, cls="t2")]
        top = y0 + 74
        for i, w in enumerate(WORKLOADS):
            out.append(text(24, top + i * row + 5, w, cls="mono", size=13))
        for (fx0, fx1), (title, vmax, ticks, unit, values) in zip(facets, specs):
            s = (fx1 - fx0 - 60) / vmax
            out.append(text(fx0, top - 26, title, size=13, weight=600))
            for t in ticks:
                x = fx0 + t * s
                out.append(f'<line class="grid" x1="{x:.1f}" y1="{top - 14}" x2="{x:.1f}" y2="{top + row * 5 - 16}"/>')
                out.append(text(x, top + row * 5 + 2, f"{t:g}{unit}", cls="t3", size=12, anchor="middle"))
            for i, w in enumerate(WORKLOADS):
                b, a, label = values[w]
                y = top + i * row
                bx, ax = fx0 + b * s, fx0 + a * s
                tip = f"{w}: {b:g}{unit} before, {a:g}{unit} after"
                out.append(f'<line class="link" x1="{bx:.1f}" y1="{y}" x2="{ax:.1f}" y2="{y}"/>')
                out.append(f'<circle class="before" cx="{bx:.1f}" cy="{y}" r="5"><title>{tip}</title></circle>')
                out.append(f'<circle class="after" cx="{ax:.1f}" cy="{y}" r="6"><title>{tip}</title></circle>')
                out.append(text(max(bx, ax) + 12, y + 4, label, size=12, weight=600))
        return out, top + row * 5 + 2

    body = [
        text(24, 38, "Optimization: before and after", size=18, weight=600),
        text(24, 61, "The baseline engine against the final code, both binaries interleaved in one session "
                     "on one machine, 100,000 timed commands per run.", cls="t2"),
        '<circle class="before" cx="30" cy="85" r="5"/>', text(42, 89, "before (baseline engine)"),
        '<circle class="after" cx="222" cy="85" r="6"/>', text(234, 89, "after (final code)"),
    ]
    panel, y = dumbbell_panel(
        134, "Mean cost per command: every workload got faster",
        "Nanoseconds per command at each binary's median run; lower is faster. Labels are the paired median "
        "speed-up; all 12 pairs per row were faster.",
        [("Engine (core), ns per command", 400, [0, 100, 200, 300, 400], "",
          {w: (*ns[(w, "core")][:2], f'{ns[(w, "core")][2]:.2f}×') for w in WORKLOADS}),
         ("Replay path (CSV through the engine), ns per command", 1100, [0, 250, 500, 750, 1000], "",
          {w: (*ns[(w, "replay")][:2], f'{ns[(w, "replay")][2]:.2f}×') for w in WORKLOADS})])
    body += panel
    panel, y = dumbbell_panel(
        y + 46, "Why: the heap left the hot path",
        "Most of the gain came from removing per-command allocation: inline result storage, a per-book node "
        "pool, an open-addressed index.",
        [("Heap's share of core time (sampling profile)", 50, [0, 10, 20, 30, 40, 50], "%",
          {w: (*heap[w], f"{heap[w][0]}% → {heap[w][1]}%") for w in WORKLOADS}),
         ("Heap allocations per timed core command (exact)", 3, [0, 1, 2, 3], "",
          {w: (round(alloc_before[w], 3), round(alloc_after[w], 3), f"{alloc_before[w]:.2f} → {alloc_after[w]:.3f}") for w in WORKLOADS})])
    body += panel
    body += [
        text(24, y + 40, "Only the ratios are the claim: the absolute figures come from short interleaved runs. "
                         "One machine, synthetic workloads, throughput only.", cls="t2"),
        text(24, y + 62, "Source: docs/OPTIMIZATION.md · benchmarks/results/2026-10-26-experiments/ · "
                         "allocations.txt in 2026-10-26-profile/ and 2026-10-26-final-profile/", cls="t3", size=12),
    ]
    write("optimization.svg", svg(width, y + 86, "Optimization: before and after",
                                  "Dumbbell charts of ns per command, heap share, and allocations per command, "
                                  "before and after optimization.", body))


# ---- replay-terminal.svg ------------------------------------------------------------------

# The terminal keeps its dark colors in both themes; only the notes beside it follow the theme.
TERMINAL_STYLE = """
  .term{fill:#161614} .bar{fill:#262622} .dot{fill:#45453f}
  .tx{font-family:ui-monospace,"Cascadia Mono",Consolas,Menlo,monospace;font-size:13px;fill:#d9d8d2}
  .prompt{fill:#7fb2f0} .cmd{fill:#f0efec;font-weight:600} .dim{fill:#8e8d86} .err{fill:#f29a9a}
  .trade{fill:#5fcfa2} .brace{stroke:#c3c2b7;stroke-width:1.5;fill:none}
  @media (prefers-color-scheme: dark){ .term{fill:#0f0f0e} .brace{stroke:#55554f} }
"""


def replay_terminal():
    expected = ROOT / "tests/replay/expected"
    good = (expected / "price_time_priority_trades_book.stdout").read_text(encoding="utf-8").splitlines()
    bad_out = (expected / "price_zero_statistics.stdout").read_text(encoding="utf-8").splitlines()
    bad_err = (expected / "price_zero.stderr").read_text(encoding="utf-8").splitlines()

    # (kind, text). The exit statuses are the ones CTest requires of these two runs.
    lines = [("prompt", "cd tests/replay"),
             ("prompt", "order_book_cli replay valid/price_time_priority.csv --trades --book")]
    lines += [("trade" if l.startswith("line ") else "", l) for l in good]
    lines += [("prompt", "echo $?"), ("", "0"),
              ("prompt", "order_book_cli replay malformed/price_zero.csv")]
    lines += [("", l) for l in bad_out] + [("err", l) for l in bad_err]
    lines += [("prompt", "echo $?"), ("", "1")]

    lh, tx0, ty0, term_w = 18, 36, 70, 600
    height = ty0 + lh * len(lines) + 24
    body = [f'<rect class="term" x="16" y="16" width="{term_w}" height="{height - 32}" rx="10"/>',
            f'<path class="bar" d="M16 26a10 10 0 0 1 10-10h{term_w - 20}a10 10 0 0 1 10 10v16h-{term_w}z"/>',
            '<circle class="dot" cx="36" cy="29" r="5"/><circle class="dot" cx="54" cy="29" r="5"/>'
            '<circle class="dot" cx="72" cy="29" r="5"/>',
            f'<text class="tx dim" x="{16 + term_w / 2}" y="33" text-anchor="middle" font-size="12">bash</text>']
    for i, (kind, line) in enumerate(lines):
        y = ty0 + i * lh
        if kind == "prompt":
            body.append(f'<text class="tx" x="{tx0}" y="{y}"><tspan class="prompt">$ </tspan>'
                        f'<tspan class="cmd">{escape(line)}</tspan></text>')
        elif line:
            cls = f"tx {kind}" if kind else "tx"
            body.append(f'<text class="{cls}" x="{tx0}" y="{y}" xml:space="preserve">{escape(line)}</text>')

    def note(first, last, heading, *more):
        top, bottom = ty0 + first * lh - 12, ty0 + last * lh + 3
        x = 16 + term_w + 16
        out = [f'<path class="brace" d="M{x} {top} h6 V{bottom} h-6"/>']
        ty = max(top + 12, (top + bottom) / 2 + 4 - 9 * len(more))
        out.append(text(x + 18, ty, heading, size=13, weight=600))
        for n, s in enumerate(more):
            out.append(text(x + 18, ty + 18 * (n + 1), s, cls="t2", size=12.5))
        return out

    trades = [i for i, (k, _) in enumerate(lines) if k == "trade"]
    stats = [i for i, (_, l) in enumerate(lines) if l.startswith("commands processed")]
    book = next(i for i, (_, l) in enumerate(lines) if l == "bids:")
    err = next(i for i, (k, _) in enumerate(lines) if k == "err")
    body += note(trades[0], trades[-1], "--trades: one line per fill",
                 "tagged with the file line of the command", "that made it (the header is line 1)")
    body += note(stats[0], stats[0] + 9, "Statistics: always printed",
                 "A command the book refuses would", "count as rejected; the replay goes on.")
    body += note(book, book + 3, "--book: the final book", "every level, best price first")
    body += note(stats[1], err, "A parse error stops the replay",
                 "Line 4 is 3,ADD,1003,BUY,0,20.", "The two commands before it ran and", "are not rolled back.",
                 "The diagnostic goes to stderr,", "and the exit status is 1.")
    write("replay-terminal.svg", svg(960, height, "Replaying a session with order_book_cli",
                                     "Terminal session replaying a valid file with --trades --book, then a "
                                     "malformed file that stops at line 4 with exit status 1.", body,
                                     TERMINAL_STYLE))


if __name__ == "__main__":
    benchmark_baseline()
    optimization()
    replay_terminal()
