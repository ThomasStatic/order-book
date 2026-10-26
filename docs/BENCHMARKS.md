# Benchmark baseline

The first recorded throughput of the order book, taken with `order_book_benchmarks` on one desktop machine. It is a reference point for later changes to this code on this machine. It is not a measure of how the engine compares with any other system. How the harness works is in [`benchmarks/README.md`](../benchmarks/README.md). This page summarizes that and records what was run, what came out, and how far the numbers can be trusted.

| | |
|---|---|
| Date | 2026-10-25 |
| Code | commit `e22a9b2` on `main`, clean working tree |
| Workload generator | version 2, seed 42 |
| Build | MSVC 19.44.35228, Release (`/O2 /Ob2 /DNDEBUG`), no automatic invariant checks, no sanitizers |
| Machine | the reference machine in [`benchmarks/README.md`](../benchmarks/README.md#environment): AMD Ryzen 5 5600G, 32 GiB DDR4-3200, Windows 11 Pro 10.0.26200, High performance power plan |
| Raw results | [`benchmarks/results/2026-10-25-baseline/`](../benchmarks/results/2026-10-25-baseline/) |

## Results

### Standard size (100,000 timed commands per run)

Three sessions, each with 1 warm-up and 30 measured runs per workload and kind, so 90 measured runs per row. Throughput is in millions of commands per second (Mops/s). Warm-ups are left out.

| Workload | Kind | Median Mops/s | ns/command at the median | p10 – p90 Mops/s | Min – max Mops/s | CV | Session medians |
|---|---|---:|---:|---|---|---:|---|
| `small-book` | core | **6.30** | 159 | 6.02 – 6.45 | 4.79 – 6.49 | 4.4% | 6.26 / 6.26 / 6.35 |
| `small-book` | replay | **1.32** | 756 | 1.21 – 1.35 | 1.04 – 1.36 | 4.7% | 1.31 / 1.34 / 1.28 |
| `deep-book` | core | **2.24** | 446 | 1.80 – 2.43 | 1.34 – 2.65 | 11.3% | 1.90 / 2.28 / 2.33 |
| `deep-book` | replay | **0.91** | 1,101 | 0.83 – 0.96 | 0.76 – 0.98 | 5.8% | 0.90 / 0.89 / 0.95 |
| `matching-heavy` | core | **3.84** | 261 | 3.33 – 4.16 | 2.19 – 4.24 | 9.7% | 3.73 / 3.85 / 4.00 |
| `matching-heavy` | replay | **1.12** | 892 | 0.98 – 1.14 | 0.85 – 1.15 | 6.4% | 1.13 / 1.12 / 1.09 |
| `cancellation-heavy` | core | **3.02** | 332 | 2.26 – 3.51 | 1.38 – 3.92 | 17.6% | 2.91 / 2.55 / 3.40 |
| `cancellation-heavy` | replay | **1.06** | 944 | 0.90 – 1.14 | 0.40 – 1.15 | 10.5% | 1.08 / 1.00 / 1.11 |
| `mostly-resting` | core | **3.31** | 302 | 2.35 – 4.24 | 1.61 – 4.37 | 22.0% | 2.75 / 3.17 / 4.07 |
| `mostly-resting` | replay | **1.14** | 881 | 1.01 – 1.18 | 0.93 – 1.19 | 6.0% | 1.11 / 1.13 / 1.15 |

- **Median** is the median of the 90 runs' throughputs. **ns/command** is 10⁹ / median: the mean cost per command within a median run. It is not the latency of any single command.
- **p10 – p90** is the range of the middle 80% of runs (see [Percentiles](#percentiles)).
- **CV** is the coefficient of variation over all 90 runs (sample standard deviation / mean).
- **Session medians** are each session's own median, in run order. They show how far a result moves between sessions on the same machine and build.

Every run's outcome matched the workload's prediction (trades, volume, final resting orders and levels, no rejections). At this size the books end with: `small-book` 173 orders on 10 levels; `deep-book` 99,555 on 2,000; `matching-heavy` 10,147 on 194 (83,586 trades); `cancellation-heavy` 49,692 on 500 (no trades); `mostly-resting` 86,874 on 100.

### Cost of the replay layers

`replay` runs the same commands as `core`, but as in-memory CSV through the parser and the replay engine, including event collection, the final snapshot, and the formatted summary. The difference at the medians is the cost of those layers per command:

| Workload | core ns/command | replay ns/command | Difference |
|---|---:|---:|---:|
| `small-book` | 159 | 756 | 597 |
| `deep-book` | 446 | 1,101 | 655 |
| `matching-heavy` | 261 | 892 | 632 |
| `cancellation-heavy` | 332 | 944 | 613 |
| `mostly-resting` | 302 | 881 | 579 |

The replay layers add roughly 580–650 ns per command whatever the workload. That is between about 1.5 and 4 times the engine's own cost, so the replay path's throughput mostly measures CSV parsing and event collection, not the book.

### Extended size (1,000,000 timed commands per run)

One session with 1 warm-up and 5 measured runs per workload and kind. Five runs are too few for percentiles, so only the median and range are given.

| Workload | Kind | Median Mops/s | ns/command at the median | Min – max Mops/s | Final book |
|---|---|---:|---:|---|---|
| `small-book` | core | 6.36 | 157 | 6.17 – 6.40 | 194 orders, 11 levels |
| `small-book` | replay | 1.27 | 787 | 1.22 – 1.33 | |
| `deep-book` | core | 1.99 | 503 | 1.73 – 2.04 | 98,611 orders, 2,001 levels |
| `deep-book` | replay | 0.92 | 1,083 | 0.86 – 0.93 | |
| `matching-heavy` | core | 4.00 | 250 | 3.76 – 4.06 | 11,201 orders, 198 levels |
| `matching-heavy` | replay | 1.12 | 896 | 0.98 – 1.13 | |
| `cancellation-heavy` | core | 3.35 | 299 | 3.21 – 3.41 | 49,808 orders, 500 levels |
| `cancellation-heavy` | replay | 1.12 | 897 | 0.94 – 1.13 | |
| `mostly-resting` | core | 2.27 | 440 | 2.23 – 2.30 | 862,963 orders, 100 levels |
| `mostly-resting` | replay | 0.93 | 1,072 | 0.86 – 0.94 | |

The four steady-state workloads keep their book's shape at this size, and every one of their medians lies within the p10 – p90 range of their standard-size runs. `mostly-resting` is the exception by design: it grows the book, here to about 863,000 orders, ten times its size at the standard size. Its cost per command rises from about 300 to 440 ns, as its working set grows about tenfold; this baseline does not isolate the cause. Its results therefore depend on the run length and are only comparable at the same size.

## Percentiles

The harness reads the clock twice per run, before the first timed command and after the last. It measures how long a run of 100,000 or 1,000,000 commands takes, not how long any one command takes. So:

- **Per-run throughput percentiles are reported**, at the standard size only. Each run's throughput is measured correctly: a run lasts at least 15 ms against a 100 ns clock tick. Ninety runs per row support p10 and p90, with nine runs beyond each. They are computed by linear interpolation between closest ranks (Python's `statistics.quantiles`, `method="inclusive"`). Higher percentiles such as p99 would rest on less than one run and are not given. At the extended size, five runs support only a median and a range.
- **Per-command latency percentiles (p50, p99, p99.9 of a single command) are not reported**, because they were not measured. Dividing a run's time by its commands gives a mean, which says nothing about the tail. Timing each command separately would need a different harness: commands here take about 150–1,100 ns, comparable to the 100 ns tick of `steady_clock` on this machine and to the cost of reading it, so the clock itself would distort the result. Any latency-percentile figure would need that harness first.

## Methodology

The harness is described fully in [`benchmarks/README.md`](../benchmarks/README.md). In short:

- **Workloads.** Five built-in workloads (`small-book`, `deep-book`, `matching-heavy`, `cancellation-heavy`, `mostly-resting`), each a preload plus a mix of limit-order submissions and cancellations with its own cancellation rate, price placement, and quantity sizes. Each is generated in full from its configuration and the seed before anything is timed, by a generator that follows its own model of the book and never touches `OrderBook`. The same generator version and seed give the same commands on every platform.
- **What is timed.** Each run builds a fresh book and applies the preload untimed. It then times every command with `std::chrono::steady_clock`, checks the outcome against the generator's prediction untimed, and destroys the book. `core` calls `submitOrder` and `cancelOrder` directly. `replay` parses the same commands as CSV text in memory through `ReplayEngine` with invariant validation off. Disk I/O is excluded from both.
- **Repetition.** One warm-up run per workload and kind, recorded in the raw files but left out of every statistic, then the measured runs. Every run starts from a fresh book.
- **This baseline.** Standard size: three back-to-back sessions of 30 measured runs, pooled to 90 per row, so the result does not rest on one session. Extended size: one session of 5 measured runs, as a check that results hold at a larger scale.
- **Statistics.** Medians rather than means, since a run disturbed by the machine is almost always slower, and the median is not pulled down by a few such runs. [`summarize.py`](../benchmarks/results/2026-10-25-baseline/summarize.py) recomputes every table on this page from the raw CSV files.

The commands that produced the raw results, run from `benchmarks/results/2026-10-25-baseline/` against the Release build of the commit above:

```bash
for s in 1 2 3; do
  order_book_benchmarks --size standard --kind both --repetitions 30 --warmup 1 --seed 42 \
    --csv standard-session$s.csv --json standard-session$s.json > standard-session$s.txt
done
order_book_benchmarks --size extended --kind both --repetitions 5 --warmup 1 --seed 42 \
  --csv extended.csv --json extended.json > extended.txt
python summarize.py
```

Each `.csv` holds every run, warm-ups included. Each `.json` holds the whole session: build, each workload's configuration, fingerprint and predicted outcome, summaries, and every run. Each `.txt` is the program's console output. Its line endings are CRLF, because the program wrote its console output in text mode on Windows until a later commit switched it to binary. The Release test suite (`ctest -C Release`, 63 tests) passed on the same build before the runs.

## Limitations

- **One machine, one compiler, one build.** Every number comes from a single Windows desktop with MSVC and CMake's default Release flags: no link-time optimization, no profile-guided optimization, no `/arch` option. GCC or Clang builds, other flags, other CPUs, or other memory systems will give different numbers, possibly ordered differently between workloads. Compare later results with these only when they are taken in the same environment.
- **Noise.** The benchmark runs on one thread, without core pinning or raised priority. The runs were launched from an agent session in a terminal, and other background activity on the machine was not controlled. The noise is visible in the results. `small-book`, whose book fits in cache, is stable (CV 4.4%, session medians within 1%). The `core` runs of the large-book workloads are not. Their session medians differ by 23% (`deep-book`), 34% (`cancellation-heavy`), and 48% (`mostly-resting`), with CVs of 11–22%. Throughput can also shift partway through a session rather than scatter around one value: in session 1, the median `deep-book` `core` run is 2.20 Mops/s over the first 12 runs and 1.82 over the last 18 (see the `.csv` files, in run order). For those workloads, a difference smaller than the spread of their session medians is not evidence of a change. Confirming a change needs several sessions of the old and new code, interleaved on a quiet machine.
- **Short runs.** A standard-size `core` run typically lasts 15–50 ms, short enough that one scheduler interruption or frequency change moves it visibly. The `replay` runs typically last 75–115 ms and are steadier, plausibly for that reason.
- **Throughput, not latency.** Nothing here measures how long one command takes, and the tail in particular (see [Percentiles](#percentiles)). The book is single-threaded and is driven from a loop in the same process: no network, no message decoding, no concurrent clients, no persistence or recovery.
- **Synthetic workloads.** The five workloads are generated mixes chosen to stress particular parts of the book: overhead, a large working set, matching, the order index, and growth. They are not recordings of real market activity. They use limit orders only (the only type the book supports), a fresh ID for every order, and a single symbol. Real order flow has bursts, correlated cancellations, and a very different price and size distribution.
- **Fresh books and allocator state.** Every run starts from a newly preloaded book, in a process whose heap has been through earlier runs. The cost of the preload is excluded, and the allocator's state carries over between runs, which may account for part of the drift in the large-book workloads.
- **`mostly-resting` grows by design**, so its numbers depend on the run length (see [Extended size](#extended-size-1000000-timed-commands-per-run)).
- **Generator version.** These results hold for workload generator version 2 and seed 42 only. A change to the generator bumps its version and makes earlier results incomparable.

## What these numbers do not show

They are not a comparison with any production exchange, trading venue, or commercial matching engine, and should not be presented as one. Such systems do different work (many symbols, order types, risk checks, networking, persistence, replication, regulatory recording) on different hardware under different load, and their published figures are measured in other ways. Nothing on this page supports a claim of being "low latency", "microsecond", or "exchange grade", or any claim about tail latency. What the numbers do support is narrower: on this machine and build, with these synthetic workloads, the engine processes a few million commands per second in one thread. They also give a reference for measuring later changes to this code in the same environment.
