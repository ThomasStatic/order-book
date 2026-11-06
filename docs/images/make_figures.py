"""Draw the data-driven figures in this directory from the files they illustrate.

Usage: python docs/images/make_figures.py   (Python 3.8+, standard library only)

- benchmark-baseline.svg: every measured standard-size run of the recorded baseline, from
  benchmarks/results/2026-10-25-baseline/standard-session{1,2,3}.csv (warm-ups excluded).
- benchmark-v2-baseline.svg: the same for the v2.0.0 baseline's eleven workloads, from
  benchmarks/results/2026-10-29-order-behaviors/standard-session{1,2,3}.csv.
- optimization.svg: the before-and-after tables of docs/OPTIMIZATION.md (ns per command and
  the heap's share of core time), and allocations per timed command from the two
  allocations.txt profiles in benchmarks/results/.
- engine-scaling.svg: the shard kind's paired ratios of N matching threads to one, from
  benchmarks/results/2026-11-06-engine/pairs/shard-*-against-1-*/summary.md.
- engine-latency.svg: p50 and p99 response time against offered rate, one panel per interval,
  from benchmarks/results/2026-11-06-engine/latency/*-ladder/report.txt.
- replay-terminal.svg: two CLI runs, drawn from tests/replay/expected/, which CTest compares
  byte for byte with the CLI's real output, so the picture cannot drift from the program.

The architecture, lifecycle, walkthrough, and ladder diagrams are edited as SVG directly. Rerun this
script after changing any file it reads.
"""

import csv
import math
import re
import statistics
from html import escape
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORKLOADS = ["small-book", "deep-book", "matching-heavy", "cancellation-heavy", "mostly-resting"]
BEHAVIOR_WORKLOADS = ["quote-refresh", "pending-stops", "self-trade", "immediate", "matching-heavy-request",
                      "matching-heavy-owned"]

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

def strip_plot(results, workloads, xmax, heading, recorded, notes, filename, desc):
    """Every measured standard-size run in benchmarks/results/<results>/, one row per workload."""
    rows = []
    for s in (1, 2, 3):
        with open(ROOT / f"benchmarks/results/{results}/standard-session{s}.csv", newline="") as f:
            rows += [r for r in csv.DictReader(f) if r["warmup"] == "false"]

    width, x0, x1, top, band = 960, 220, 680, 140, 66
    scale = (x1 - x0) / xmax
    bottom = top + band * len(workloads)
    body = [
        text(24, 38, heading, size=18, weight=600),
        text(24, 61, "Throughput of each run of 100,000 timed commands, 90 runs per row (3 sessions × 30). "
                     "Each dot is one run; the bar is the median.", cls="t2"),
        text(24, 80, recorded, cls="t2"),
        '<circle class="core" cx="30" cy="105" r="5"/>', text(42, 109, "core: OrderBook called directly"),
        '<circle class="replay" cx="262" cy="105" r="5"/>',
        text(274, 109, "replay: the same commands as in-memory CSV through the replay engine"),
        text(x1 + 30, top - 10, "median", cls="t3", size=12), text(x1 + 120, top - 10, "CV", cls="t3", size=12),
        text(x1 + 170, top - 10, "min–max", cls="t3", size=12),
    ]
    for tick in range(0, int(xmax) + 1):
        x = x0 + tick * scale
        body.append(f'<line class="grid" x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{bottom}"/>')
        body.append(text(x, bottom + 18, str(tick), cls="t3", size=12, anchor="middle"))
    body.append(text((x0 + x1) / 2, bottom + 38, "millions of commands per second", cls="t3", size=12,
                     anchor="middle"))

    for i, w in enumerate(workloads):
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
    for k, note in enumerate(notes):
        body.append(text(24, y + 20 * k, note, cls="t2"))
    y += 20 * (len(notes) - 1)
    body.append(text(24, y + 24, f"Source: benchmarks/results/{results}/ · tables in docs/BENCHMARKS.md",
                     cls="t3", size=12))
    write(filename, svg(width, y + 46, "Benchmark baseline", desc, body))


def benchmark_baseline():
    strip_plot("2026-10-25-baseline", WORKLOADS, 7.0, "Benchmark baseline: every run, before optimization",
               "Recorded 2026-10-25 on one machine (Ryzen 5 5600G, MSVC Release). Higher is faster.",
               ["Replay sits near 1 Mops/s on every workload: CSV parsing and event collection cost about "
                "600 ns per command, 1.5–4× the engine's own cost.",
                "The large-book core rows scatter widely (CV 11–22%), so later changes were judged by "
                "interleaved A/B pairs, not against these runs."],
               "benchmark-baseline.svg", "Strip plot of per-run throughput for five workloads, core and replay.")


V2_XMAX = 12.0
V2_NOTES = [
    "The v1 workloads run 1.5–1.8× their first baseline on core and 2.0–2.4× on replay (docs/OPTIMIZATION.md).",
    "The behavior workloads' replay rows are slower: twelve-column v2 rows, and more events per command to collect.",
]


def behaviors_baseline():
    strip_plot("2026-10-29-order-behaviors", WORKLOADS + BEHAVIOR_WORKLOADS, V2_XMAX,
               "Benchmark baseline, v2.0.0: every run, all eleven workloads",
               "Recorded 2026-10-29 on the same machine (Ryzen 5 5600G, MSVC Release), after optimization and "
               "the order behaviors. Higher is faster.",
               V2_NOTES, "benchmark-v2-baseline.svg",
               "Strip plot of per-run throughput for eleven workloads, core and replay.")


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


# ---- engine-scaling.svg and engine-latency.svg ------------------------------------------------

ENGINE = "benchmarks/results/2026-11-06-engine"
THREADED = ["quote-refresh", "deep-book", "immediate", "matching-heavy-request", "pending-stops"]
# The palette's first five categorical slots, validated for both surfaces; series carry direct
# labels and marker shapes too, since three of the light steps sit under 3:1 against the surface.
SERIES_STYLE = """
  .s1{stroke:#2a78d6;fill:#2a78d6} .s2{stroke:#eb6834;fill:#eb6834} .s3{stroke:#1baf7a;fill:#1baf7a}
  .s4{stroke:#eda100;fill:#eda100} .s5{stroke:#e87ba4;fill:#e87ba4}
  .line{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
  .ideal{fill:none;stroke:#c3c2b7;stroke-width:1.5;stroke-dasharray:5 4}
  .ci{stroke-width:1.5;stroke-linecap:round} .ring{stroke:#fcfcfb;stroke-width:2}
  .hollow{fill:#fcfcfb;stroke-width:2}
  @media (prefers-color-scheme: dark){
    .s1{stroke:#3987e5;fill:#3987e5} .s2{stroke:#d95926;fill:#d95926} .s3{stroke:#199e70;fill:#199e70}
    .s4{stroke:#c98500;fill:#c98500} .s5{stroke:#d55181;fill:#d55181}
    .ideal{stroke:#4a4a44} .ring{stroke:#1a1a19} .hollow{fill:#1a1a19}
  }
"""


def pair_row(name):
    """compare_ab.py's summary.md row for one workload: (median, low, high)."""
    for line in (ROOT / ENGINE / "pairs" / name / "summary.md").read_text(encoding="utf-8").splitlines():
        m = re.match(r"^\| (\S+) \| \S+ \| ([0-9.]+) \| ([0-9.]+) - ([0-9.]+) \|", line.strip())
        if m:
            return float(m.group(2)), float(m.group(3)), float(m.group(4))
    raise ValueError(f"no row in pairs/{name}")


def marker(shape, cx, cy, cls, r=4.5):
    if shape == 0:
        return f'<circle class="{cls} ring" cx="{cx:.1f}" cy="{cy:.1f}" r="{r}"/>'
    if shape == 1:
        return f'<rect class="{cls} ring" x="{cx - r:.1f}" y="{cy - r:.1f}" width="{2 * r}" height="{2 * r}" rx="1.5"/>'
    if shape == 2:
        return (f'<path class="{cls} ring" d="M{cx:.1f} {cy - r - 1:.1f}L{cx + r + 1:.1f} {cy + r:.1f}'
                f'L{cx - r - 1:.1f} {cy + r:.1f}z"/>')
    if shape == 3:
        return (f'<path class="{cls} ring" d="M{cx:.1f} {cy - r - 1:.1f}L{cx + r + 1:.1f} {cy:.1f}'
                f'L{cx:.1f} {cy + r + 1:.1f}L{cx - r - 1:.1f} {cy:.1f}z"/>')
    # A cross is all stroke, so it takes the series color with no surface ring.
    return (f'<path class="{cls}" d="M{cx - r:.1f} {cy - r:.1f}L{cx + r:.1f} {cy + r:.1f}'
            f'M{cx + r:.1f} {cy - r:.1f}L{cx - r:.1f} {cy + r:.1f}" stroke-width="2.5" stroke-linecap="round"/>')


def engine_scaling():
    """shard at N matching threads against N = 1, each its own physical core: the paired ratios."""
    counts = [1, 2, 3, 4, 6]
    width, x0, x1, top, y1 = 960, 90, 700, 120, 520
    xs = lambda n: x0 + (n - 1) / 5 * (x1 - x0)
    ys = lambda v: y1 - (v - 0) / 6 * (y1 - top)
    body = [
        text(24, 38, "Throughput against matching threads, shard kind", size=18, weight=600),
        text(24, 61, "Paired ratio of N matching threads to one, 16 ABBA pairs each, extended size: 1M commands per shard, "
                     "8 instruments per shard,", cls="t2"),
        text(24, 80, "uniform spread, even placement, no log, spin window 100us, each thread on its own physical core at high "
                     "priority. Windows 11, MSVC Release.", cls="t2"),
    ]
    for v in range(0, 7):
        y = ys(v)
        body.append(f'<line class="grid" x1="{x0}" y1="{y:.1f}" x2="{x1}" y2="{y:.1f}"/>')
        body.append(text(x0 - 10, y + 4, f"{v}×", cls="t3", size=12, anchor="end"))
    for n in counts:
        body.append(text(xs(n), y1 + 20, str(n), cls="t3", size=12, anchor="middle"))
    body.append(text((x0 + x1) / 2, y1 + 42, "matching threads N, one physical core each (the machine has 6)",
                     cls="t3", size=12, anchor="middle"))
    body.append(f'<line class="axis" x1="{x0}" y1="{y1}" x2="{x1}" y2="{y1}"/>')
    body.append(f'<path class="ideal" d="M{xs(1):.1f} {ys(1):.1f}L{xs(6):.1f} {ys(6):.1f}"/>')
    body.append(text(xs(6) - 6, ys(6) - 8, "N× (perfect scaling)", cls="t3", size=12, anchor="end"))
    ends = []
    for i, w in enumerate(THREADED):
        cls = f"s{i + 1}"
        points = [(1, 1.0, 1.0, 1.0)] + [(n, *pair_row(f"shard-{n}-against-1-{w}")) for n in counts[1:]]
        d = "M" + "L".join(f"{xs(n):.1f} {ys(v):.1f}" for n, v, _, _ in points)
        body.append(f'<path class="{cls} line" d="{d}"/>')
        for n, v, lo, hi in points[1:]:
            body.append(f'<line class="{cls} ci" x1="{xs(n):.1f}" y1="{ys(lo):.1f}" x2="{xs(n):.1f}" y2="{ys(hi):.1f}"/>')
        for n, v, lo, hi in points:
            body.append(marker(i, xs(n), ys(v), cls))
        ends.append((ys(points[-1][1]), w, points[-1][1], cls, i))
    # Direct labels at the right, nudged apart so none overlap.
    ends.sort()
    placed = []
    for y, w, v, cls, i in ends:
        y = max(y, placed[-1] + 17) if placed else y
        placed.append(y)
        body.append(marker(i, x1 + 22, y - 4, cls, r=4))
        body.append(text(x1 + 34, y, f"{w}  {v:.2f}×", size=12.5))
    body.append(text(24, y1 + 74, "Each ratio's 95% bootstrap interval is within ±0.06 of it, drawn as a bar mostly hidden by its marker. "
                                  "No point goes past 6 physical cores.", cls="t2"))
    body.append(text(24, y1 + 94, "Tables, the other kinds, and every cell's run record: docs/BENCHMARKS.md · "
                                  f"Source: {ENGINE}/pairs/shard-*-against-1-*", cls="t3", size=12))
    write("engine-scaling.svg", svg(width, y1 + 116, "Engine scaling",
                                    "Line chart of the shard kind's throughput at 2, 3, 4, and 6 matching threads, "
                                    "each on its own physical core, as a paired ratio to one thread, for five "
                                    "workloads, beside the perfect-scaling diagonal.", body, SERIES_STYLE))


def latency_ladder(row):
    """Each open-loop rung of latency/<row>/report.txt: (percent of capacity, achieved rate, p50, p99),
    each percentile (ns, marked) or None, a rung that can't be quoted left out."""
    rungs = []
    for block in (ROOT / ENGINE / "latency" / row / "report.txt").read_text(encoding="utf-8").split("\n\n"):
        lines = block.strip().splitlines()
        if not lines or not lines[0].startswith("==") or "to the first frame" in lines[0] or "lateness" in lines[0]:
            continue
        m = re.search(r"fixed at (\d+)%", lines[0])
        if not m or not any(l.strip() == "quotable: yes" for l in lines):
            continue
        rate = next(int(re.search(r"(\d+)/s", l).group(1)) for l in lines if l.strip().startswith("achieved-rate="))
        values = {}
        for l in lines:
            p = re.match(r"^(p50|p99)\s+(\d+)\s.*?((?:\s+(?:noise|unresolved))*)\s*$", l)
            if p:
                values[p.group(1)] = (int(p.group(2)), bool(p.group(3).strip()))
        rungs.append((int(m.group(1)), rate, values.get("p50"), values.get("p99")))
    return rungs


def engine_latency():
    """Response time against offered rate, one panel per interval, p50 and p99 on a log scale."""
    panels = [
        ("command-ladder-matching-heavy-request", "Command", "one book call"),
        ("hand-off-ladder", "Hand-off", "into the command ring, back out of the response ring"),
        ("durable-wait-process-ladder", "Durable wait, process", "log ring to the durable position"),
        ("durable-wait-machine-ladder", "Durable wait, machine", "log ring to the durable position"),
        ("round-trip-process-ladder", "Round trip, process", "over loopback through the shipped engine"),
        ("round-trip-machine-ladder", "Round trip, machine", "over loopback through the shipped engine"),
    ]
    width, pw, ph, gx, gy, left, top = 960, 236, 190, 72, 96, 76, 164
    decades = [100, 1_000, 10_000, 100_000, 1_000_000, 10_000_000, 100_000_000, 1_000_000_000]
    names = ["100 ns", "1 µs", "10 µs", "100 µs", "1 ms", "10 ms", "100 ms", "1 s"]
    body = [
        text(24, 38, "Response time against offered rate, matching-heavy-request", size=18, weight=600),
        text(24, 61, "Each interval at 10, 25, 50, 75, and 90% of the capacity it measured that session, fixed timetable, "
                     "9 runs merged. Log scale.", cls="t2"),
        text(24, 80, "Pinned at high priority, one shard, one connection; durability as named; one drive. Windows 11, "
                     "MSVC Release, rdtscp clock.", cls="t2"),
        f'<path class="s1 line" d="M30 105L54 105"/>', marker(0, 42, 105, "s1", r=4), text(62, 109, "p50"),
        f'<path class="s2 line" d="M110 105L134 105"/>', marker(1, 122, 105, "s2", r=4), text(142, 109, "p99"),
        '<circle class="s1 hollow" cx="196" cy="105" r="4.5"/>',
        text(208, 109, "hollow: marked noise or unresolved in its report; a missing rung was not sustained or not quotable",
             cls="t2", size=12.5),
    ]
    for k, (row, title, what) in enumerate(panels):
        px, py = left + (k % 3) * (pw + gx), top + (k // 3) * (ph + gy)
        rungs = latency_ladder(row)
        values = [v[0] for r in rungs for v in (r[2], r[3]) if v]
        lo = max(d for d in decades if d <= min(values))
        hi = min(d for d in decades if d >= max(values))
        span = math.log10(hi) - math.log10(lo)
        ys = lambda v: py + ph - (math.log10(v) - math.log10(lo)) / span * ph
        xs = lambda pct: px + (pct - 10) / 80 * pw
        body.append(text(px, py - 26, title, size=13.5, weight=600))
        body.append(text(px, py - 10, what, cls="t3", size=11.5))
        for d, label in zip(decades, names):
            if lo <= d <= hi:
                body.append(f'<line class="grid" x1="{px}" y1="{ys(d):.1f}" x2="{px + pw}" y2="{ys(d):.1f}"/>')
                body.append(text(px - 8, ys(d) + 4, label, cls="t3", size=11, anchor="end"))
        for pct in (10, 25, 50, 75, 90):
            body.append(text(xs(pct), py + ph + 16, f"{pct}%", cls="t3", size=11, anchor="middle"))
        rates = ", ".join(f"{r[1]:,}" for r in rungs)
        body.append(text(px, py + ph + 32, f"per second: {rates}", cls="t3", size=10))
        for index, (cls, shape) in ((2, ("s1", 0)), (3, ("s2", 1))):
            pts = [(r[0], r[index]) for r in rungs if r[index]]
            if len(pts) > 1:
                d = "M" + "L".join(f"{xs(p):.1f} {ys(v[0]):.1f}" for p, v in pts)
                body.append(f'<path class="{cls} line" d="{d}"/>')
            for p, (v, marked) in pts:
                if marked:
                    body.append(f'<circle class="{cls} hollow" cx="{xs(p):.1f}" cy="{ys(v):.1f}" r="4.5"/>')
                else:
                    body.append(marker(shape, xs(p), ys(v), cls, r=4))
    bottom = top + 2 * ph + gy + 40
    body.append(text(24, bottom + 10, "Each panel has its own scale; no interval is subtracted from another, and no max is "
                                      "shown. Tables with each row's run record: docs/LATENCY.md.", cls="t2"))
    body.append(text(24, bottom + 30, f"Source: {ENGINE}/latency/*-ladder/report.txt", cls="t3", size=12))
    write("engine-latency.svg", svg(width, bottom + 52, "Engine latency against rate",
                                    "Six small line charts, one per interval, of p50 and p99 response time on a log "
                                    "scale against the offered rate as a share of the capacity each measured.",
                                    body, SERIES_STYLE))


if __name__ == "__main__":
    benchmark_baseline()
    behaviors_baseline()
    optimization()
    replay_terminal()
    engine_scaling()
    engine_latency()
