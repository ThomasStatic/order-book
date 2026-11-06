# Benchmark baselines

Recorded throughput of the order book, taken with `order_book_benchmarks` on one desktop machine. Each baseline is a reference point for later changes to this code on this machine. It is not a measure of how the engine compares with any other system. How the harness works is in [`benchmarks/README.md`](../benchmarks/README.md). This page records what was run, what came out, and how far the numbers can be trusted.

There are three. The current one, for v2.1.0, is the engine set: the book against v2.0.0 on all eleven workloads, and the threaded kinds, from one shard to six, with and without a log. The v2.0.0 baseline covers all eleven workloads on the book alone: the five v1 workloads and the six behavior workloads. The first, from 2026-10-25, predates both the optimizations in [`OPTIMIZATION.md`](OPTIMIZATION.md) and the order behaviors, and is kept below as history. The two are not comparable as a before and after: the code, the generator version, and the index's sizing hint all changed between them. The before-and-after comparisons are paired, interleaved runs of two binaries, in [`OPTIMIZATION.md`](OPTIMIZATION.md).

## The v2.1.0 engine set (2026-11-06)

| | |
|---|---|
| Date | 2026-11-06, one unattended session from 01:32 to 13:54, 754 steps, none failed |
| Code | commit `2affa3b` on `main` |
| Workload generator | version 3, seed 42 |
| Build | MSVC 19.44.35228, Release, with the Visual Studio 17 2022 generator |
| OS | Windows 11 Pro 10.0.26200, High performance power plan |
| Machine | the reference machine of [`benchmarks/README.md`](../benchmarks/README.md#environment): AMD Ryzen 5 5600G, 6 physical cores with SMT, one NVMe drive |
| Raw results | [`benchmarks/results/2026-11-06-engine/`](../benchmarks/results/2026-11-06-engine/); its `summarize.py` recomputes every table here, and its README lists what each directory holds |

Every threaded cell below shares these settings unless its row says otherwise:
- **Instruments.** 8 a shard.
- **Spread and placement.** A uniform spread, with even placement (instrument i on shard i mod N).
- **Spin window.** `100us`.
- **Threads.** Each thread is pinned at high priority, on a physical core of its own (layout `one-core-each`).
- **Size.** The extended size: 1M timed commands a shard.

Rows whose layout is `siblings` or `producer-on-sibling` put threads on a core's second logical processor, and they say so. **No scaling claim here goes past the machine's 6 physical cores.**

Each cell's figure is the median over three sessions of 5 measured runs each. The paired figures are ABBA ratios, 16 pairs, with a 95% bootstrap interval. The A/A floors, the same binary on both sides first, are:

| Kind | Floor |
|---|---:|
| `shard` | ±1.9% |
| `queue` | ±1.4% |
| `gateway` | ±0.4% |
| `engine` | ±1.4% |

The kinds, from the book outward (`benchmarks/README.md` has each):

| Kind | What runs |
|---|---|
| `core` | `OrderBook` called directly |
| `shard` | Each shard's books on a matching thread of their own, with commands already in its loop |
| `queue` | Producer threads that feed each shard through its rings |
| `gateway` | Loopback clients that send frames to gateway threads |
| `engine` | The shipped `order_book_engine` process |

### The gate: `core` against v2.0.0

v2.1.0 may be no more than 3% slower than tag v2.0.0 on any of the eleven workloads, at both sizes, or the exception is explained and accepted.

| Workload | Standard, now / v2.0.0 | 95% CI | Extended, now / v2.0.0 | 95% CI |
|---|---:|---|---:|---|
| `small-book` | 0.982 | 0.966 - 0.990 | 0.981 | 0.975 - 0.994 |
| `deep-book` | 1.088 | 1.050 - 1.134 | 1.045 | 1.033 - 1.121 |
| `matching-heavy` | 1.052 | 1.043 - 1.064 | 1.047 | 1.041 - 1.050 |
| `cancellation-heavy` | 1.069 | 0.933 - 1.195 | 1.032 | 0.987 - 1.043 |
| `mostly-resting` | 1.277 | 1.216 - 1.330 | 1.093 | 1.089 - 1.100 |
| `quote-refresh` | **0.963** | 0.952 - 0.967 | **0.964** | 0.961 - 0.973 |
| `pending-stops` | 1.132 | 1.122 - 1.140 | 1.144 | 1.134 - 1.160 |
| `self-trade` | 1.087 | 1.055 - 1.097 | 1.090 | 1.085 - 1.098 |
| `immediate` | 1.082 | 1.074 - 1.087 | 1.096 | 1.090 - 1.101 |
| `matching-heavy-request` | 1.076 | 1.065 - 1.081 | 1.068 | 1.062 - 1.072 |
| `matching-heavy-owned` | 1.067 | 1.061 - 1.070 | 1.066 | 1.054 - 1.070 |

**`quote-refresh` is the one exception, explained and accepted.** The results' `gate-bisect/` reads it at 13 commits.
- **From `92a1d24` to `522461c`:** 0.979 to 0.987, the cost of result reuse (L4) that L5 had already flagged at 0.974.
- **From V2a's commit `68601c6` on:** 0.959 to 0.961.

The step isn't the engine:
- **The library.** Every object file of `order_book` disassembles to the same instructions on both sides of it.
- **The timed code.** The benchmark's timed loop does too.

The same code links at different addresses, and `quote-refresh`, the cheapest command per operation, shows that most. The real 1.3 to 2.1% is [#107](https://github.com/ThomasStatic/order-book/issues/107).

`replay`, which parses CSV in the timed region, is reported and not gated: 0.933 to 1.031 of v2.0.0, lowest on `quote-refresh` and `small-book`.

### Scaling across shards

![Paired throughput ratio of N matching threads to one, shard kind, five workloads, against the N× diagonal: 3.46 to 5.26 times at N = 6.](images/engine-scaling.svg)

These cells are `shard` at N matching threads. The layout is `one-core-each`, with matching threads on logical processors 2, 4, 6, 8, 10, and 0, one per physical core. There's no log. Each figure is the paired ratio to N = 1, then the session median at N.

| Workload | N = 2 | N = 3 | N = 4 | N = 6 | Ops/s at N = 1 | Ops/s at N = 6 |
|---|---:|---:|---:|---:|---:|---:|
| `quote-refresh` | 1.950 | 2.871 | 3.766 | 5.257 | 6.23M | 33.17M |
| `deep-book` | 1.865 | 2.741 | 3.524 | 4.939 | 2.80M | 13.47M |
| `immediate` | 1.905 | 2.625 | 3.171 | 4.081 | 4.31M | 17.11M |
| `matching-heavy-request` | 1.878 | 2.542 | 3.092 | 4.101 | 4.49M | 18.18M |
| `pending-stops` | 1.729 | 2.211 | 2.679 | 3.460 | 3.08M | 10.54M |

Every interval is within ±0.06 of its median, and every pair was faster at N than at 1. The shards share nothing but the machine: memory bandwidth, the shared L3, and the other cores' heat. Each workload's ratio falls away from N in its own way. `pending-stops` scales least. Unpinned at N = 2, `shard` reads within 2.5% of pinned on every workload.

### The kinds side by side, N = 1

Paired, the outer kind over the inner one, on the same books:

| Workload | `shard` / `core` | `queue` / `shard` | `gateway` / `queue` | `engine` / `gateway`, unpinned, `process` |
|---|---:|---:|---:|---:|
| `quote-refresh` | 0.831 | 0.926 | 0.732 | 1.013 (24 pairs) |
| `deep-book` | 0.785 | 0.962 | 1.075 | 0.993 (24 pairs) |
| `immediate` | 0.919 | 0.838 | 1.126 | 1.005 (24 pairs) |
| `matching-heavy-request` | 0.914 | 0.850 | 1.096 | 1.006 |
| `pending-stops` | 0.961 | 0.907 | 1.205 | 1.019 |

- **`shard` / `core`.** `core` can't pin itself, so only the `shard` side is placed. `shard` is 17 to 22% below the book called directly on `deep-book` and `quote-refresh`, the cheapest commands per operation. That's [#101](https://github.com/ThomasStatic/order-book/issues/101).
- **`gateway` / `queue`.** `gateway` reads above `queue` on four workloads, likely because `queue`'s producer counts events in the timed region, as [#102](https://github.com/ThomasStatic/order-book/issues/102) noted. `quote-refresh` is the exception, where the wire costs more than matching. At window 1, one command in flight on one instrument, `gateway` runs 101.8k commands/s on `quote-refresh`.
- **`engine` / `gateway`.** The shipped engine reads within 2% of the in-process `gateway` kind. The kinds were run unpinned on both sides, since this program can't place the shipped engine's threads.

### Producers, gateway threads, and siblings

Session medians in commands/s, with no log:

| Kind | N | M | Instruments | Layout | `quote-refresh` | `matching-heavy-request` | `pending-stops` |
|---|---:|---:|---:|---|---:|---:|---:|
| `queue` | 1 | 1 | 8 | one-core-each | 5.53M | 3.79M | 2.79M |
| `queue` | 2 | 1 | 16 | one-core-each | 11.20M | 7.18M | 4.70M |
| `queue` | 2 | 2 | 16 | one-core-each | 10.76M | 7.03M | 4.59M |
| `queue` | 2 | 2 | 16 | **producer-on-sibling** | 9.98M | 6.14M | 4.07M |
| `queue` | 3 | 1 | 24 | one-core-each | 17.11M | 9.85M | 6.11M |
| `queue` | 3 | 3 | 24 | one-core-each | 15.80M | 9.27M | 5.89M |
| `queue` | 3 | 3 | 24 | **producer-on-sibling** | 14.48M | 8.23M | 5.37M |
| `queue` | 4 | 4 | 32 | **producer-on-sibling** | 18.64M | 10.49M | 6.72M |
| `queue` | 6 | 6 | 48 | **producer-on-sibling** | 25.05M | 14.34M | 8.82M |
| `gateway` | 1 | 1 | 8 | one-core-each | 4.17M | 4.08M | 3.36M |
| `gateway` | 2 | 1 | 16 | one-core-each | 4.35M | 4.23M | 3.97M |
| `gateway` | 2 | 2 | 16 | one-core-each | 8.33M | 7.81M | 5.09M |
| `gateway` | 4 | 1 | 32 | one-core-each | 4.62M | 4.32M | 4.01M |
| `gateway` | 4 | 2 | 32 | **siblings** | 8.42M | 7.82M | 6.26M |
| `engine` | 1 | 1 | 8 | unpinned, `process` | 4.13M | 3.85M | 3.00M |
| `engine` | 2 | 1 | 16 | unpinned, `process` | 4.31M | 4.13M | 3.71M |

- **One gateway thread** carries 2.9 to 4.6M commands/s across the five workloads, whatever N is behind it. At N = 2, a second raises that 1.92 times on `quote-refresh`, 1.85 times on `matching-heavy-request`, and 1.28 times on `pending-stops`.
- **The lowest thread CPU share in `gateway` rows** falls to 0.51 to 0.62 at N = 4 and M = 1. That's consistent with matching threads sleeping between commands one gateway thread can't send fast enough. The record names the lowest share, not which thread had it.
- **A producer on its matching thread's sibling** costs 7 to 13% against one on a core of its own, at N = 2 and 3.
- **At N = 6 with producers on siblings,** the lowest thread CPU share falls to 0.50 to 0.57 on three of the five workloads, against 0.86 to 1.00 in every other `queue` row. Every logical processor runs a thread there. Read that row as a machine at its limit, not as six shards' worth.

### Durability

`queue` at N = 1 and M = 1, paired against itself:
- **Writing a log, at `process` durability:** 0.744 to 0.766 of no log on all five workloads.
- **`machine` against `process`:**

  | Workload | `machine` / `process` | 95% CI |
  |---|---:|---|
  | `deep-book` | 0.970 | 0.965 - 0.984 |
  | `pending-stops` | 1.014 | 1.001 - 1.034, 24 pairs |
  | `quote-refresh` | 1.060 | 0.953 - 1.086 |
  | `matching-heavy-request` | 1.060 | 1.053 - 1.073 |
  | `immediate` | 1.081 | 1.065 - 1.087 |

  In `queue` the matching thread never waits for durability ([ADR 0013](adr/0013-a-response-waits-for-the-log-but-matching-does-not.md)). So the drive's flushes slow the log writer, not the matching thread, and `machine` costing nothing or less is consistent with that. No ticket gave a reason it contradicts.

Session medians in commands/s, extended files as the engine makes them against zero-filled:

| Kind | N | M | Durability | Files | `quote-refresh` | `deep-book` |
|---|---:|---:|---|---|---:|---:|
| `queue` | 1 | 1 | `process` | extended | 4.27M | 2.06M |
| `queue` | 1 | 1 | `process` | zero-filled | 4.27M | 2.11M |
| `queue` | 1 | 1 | `machine` | extended | 1.25M | 2.04M |
| `queue` | 1 | 1 | `machine` | zero-filled | 4.71M | 2.09M |
| `queue` | 2 | 2 | `machine` | extended | 8.48M | 3.72M |
| `gateway` | 1 | 1 | `machine` | extended | 4.16M | 2.06M |
| `gateway` | 2 | 1 | `machine` | extended | 4.47M | 3.87M |

`queue` on `quote-refresh` at `machine` on extended files, 1.25M/s, is the one cell where the drive sets the rate, and unevenly: its three sessions read 1.25M, 0.63M, and 4.58M. Each record of that workload is cheap enough to matter, and an extended file's write-through stalls on this drive ([`LATENCY.md`](LATENCY.md#10ks-durable-wait)). On zero-filled files the same cell reads 4.71M/s.

**A roll-forward worker** (`--roll-worker on`, the `long` size, 20M commands a shard) nearly halves `queue`: 0.531 to 0.568 at N = 1, paired. That reverses D4's reason, and it's [#108](https://github.com/ThomasStatic/order-book/issues/108). In this layout the worker shares the log writer's logical processor, which is the matching thread's sibling, and the lowest thread CPU share falls from about 0.95 to about 0.45. The shipped engine doesn't pin its roll threads.

### Books, spread, and the mix

Session medians in commands/s. `shard` and `queue` run at N = 1 and M = 1 (`queue` only) as the number of books on the shard grows:

| Books | `shard`, `quote-refresh` | `shard`, `matching-heavy-request` | `queue`, `quote-refresh` | `queue`, `matching-heavy-request` |
|---:|---:|---:|---:|---:|
| 1 | 8.93M | 5.72M | 6.26M | 4.05M |
| 8 | 6.20M | 4.47M | 5.58M | 3.83M |
| 64 | 3.60M | 2.64M | 3.54M | 2.49M |
| 512 | 2.02M | 1.94M | 2.13M | 1.94M |

More books on one matching thread means less of each in cache. At 512 the two kinds read within 6% of each other, unpaired.

**Skewed spreads.** Zipf with exponent 1 over 32 instruments, on `quote-refresh`:

| Kind | N | M | Layout | Placement `hash` | Placement `hot-alone` |
|---|---:|---:|---|---:|---:|
| `shard` | 4 | 1 | one-core-each | 18.53M | 19.69M |
| `queue` | 3 | 3 | one-core-each | 10.40M | 12.30M |
| `gateway` | 4 | 2 | **siblings** | 5.80M | 5.85M |

**The mix.** `mixed` puts two instruments of each shape on every shard. At N = 2 it reads:

| Kind | Layout | Commands/s |
|---|---|---:|
| `shard` | one-core-each | 6.81M |
| `queue` | one-core-each, M = 2 | 6.17M |
| `gateway` | one-core-each, M = 1 | 4.32M |

### The gateway's window, connections, and frame budget

`gateway` at N = 1 and M = 1 on `quote-refresh`, session medians:

| Window | 1 | 16 | 256 | 4,096 |
|---|---:|---:|---:|---:|
| Commands/s, 1 instrument | 101.8k | 1.16M | 3.59M | 3.91M |

| Connections, one instrument each | 1 | 4 | 16 | 64 |
|---|---:|---:|---:|---:|
| Commands/s | 3.58M | 3.99M | 4.38M | 3.77M |

| Frame budget, 16 instruments | 64 | unlimited |
|---|---:|---:|
| Commands/s | 4.42M | 5.08M |

The shipped budget, 64 frames a connection a pass, keeps one busy connection from holding a gateway thread at 0.87 of the unlimited rate here. That fairness is the reason it exists, not throughput.

### Limitations of the engine set

- **One machine, one drive.** The engine set comes from one Windows desktop, with MSVC and CMake's default Release flags. Its one NVMe drive holds the logs, the checkpoints, and the system. No second drive was measured.
- **Loopback, not a network.** `gateway` and `engine` clients run on the same machine, over loopback, and take cores the engine could otherwise use.
- **Synthetic, uniform load.** The workloads are the generated mixes of `benchmarks/README.md`, one stream a book. Only the skewed rows spread commands unevenly.
- **Six physical cores.** Nothing here says what a seventh would do. Rows with threads on siblings are labelled and are not scaling points.
- **The rest of the limitations below** apply too, except noise: every threaded cell here is pinned at high priority, with the A/A floors above.

### Linux

No Linux numbers are recorded for v2.1.0. Linux is built and tested in CI. The manual Linux sanity workflow (`.github/workflows/linux-sanity.yml`) checks for large effects only, on shared GitHub runners, and its runs are never quoted.

### How it was run

From the repository root, against the Release builds of `2affa3b`, of tag v2.0.0 (for the gate), and of `17a8013` (v2.0.0's library in the latency and component programs):

```bash
python benchmarks/record_baseline.py --out benchmarks/results/2026-11-06-engine \
  --machine-record benchmarks/results/2026-11-05-latency-v1/machine.txt \
  --base <v2.0.0>/order_book_benchmarks --before-latency <17a8013>/order_book_latency_benchmarks \
  --before-component <17a8013>/order_book_component_benchmarks
python benchmarks/record_baseline.py ... --only pairs --recheck <pair> ...   # each pair near its floor, at 24 pairs
python benchmarks/results/2026-11-06-engine/summarize.py --out benchmarks/results/2026-11-06-engine
```

`record_baseline.py --list` prints every step. The latency rows from the same session are in [`LATENCY.md`](LATENCY.md).

## The v2.0.0 baseline (2026-10-29)

| | |
|---|---|
| Date | 2026-10-29 |
| Code | commit `f7db5b5` on `main`; v2.0.0 adds only the code review's behavior-preserving fixes after it (a shared level-handle check and a shared amount check) |
| Workload generator | version 3, seed 42 |
| Build | MSVC 19.44.35228, Release (`/O2 /Ob2 /DNDEBUG`), no automatic invariant checks, no sanitizers |
| Machine | the reference machine in [`benchmarks/README.md`](../benchmarks/README.md#environment), as for the first baseline |
| Raw results | [`benchmarks/results/2026-10-29-order-behaviors/`](../benchmarks/results/2026-10-29-order-behaviors/) |

![Strip plot of every measured standard-size run of the v2.0.0 baseline: 90 per workload and kind, eleven workloads, with each row's median, CV, and range.](images/benchmark-v2-baseline.svg)

### Standard size (100,000 timed commands per run)

Three sessions, each with 1 warm-up and 30 measured runs per workload and kind, so 90 measured runs per row. Throughput is in millions of commands per second (Mops/s). Warm-ups are left out. The columns are as in the first baseline ([below](#standard-size-100000-timed-commands-per-run-1)).

| Workload | Kind | Median Mops/s | ns/command at the median | p10 – p90 Mops/s | Min – max Mops/s | CV | Session medians |
|---|---|---:|---:|---|---|---:|---|
| `small-book` | core | **11.21** | 89 | 10.75 – 11.41 | 9.70 – 11.49 | 2.7% | 11.21 / 11.16 / 11.37 |
| `small-book` | replay | **3.23** | 310 | 2.86 – 3.34 | 1.82 – 3.40 | 8.4% | 3.27 / 3.00 / 3.30 |
| `deep-book` | core | **4.12** | 243 | 3.79 – 4.32 | 3.33 – 4.47 | 5.5% | 4.17 / 3.94 / 4.17 |
| `deep-book` | replay | **2.01** | 498 | 1.85 – 2.07 | 1.71 – 2.11 | 4.8% | 1.99 / 2.01 / 2.01 |
| `matching-heavy` | core | **5.83** | 171 | 5.43 – 5.99 | 5.05 – 6.14 | 4.2% | 5.87 / 5.84 / 5.79 |
| `matching-heavy` | replay | **2.34** | 427 | 2.25 – 2.40 | 2.08 – 2.44 | 2.9% | 2.37 / 2.34 / 2.33 |
| `cancellation-heavy` | core | **5.18** | 193 | 3.02 – 5.98 | 1.78 – 6.57 | 24.3% | 3.59 / 5.83 / 5.32 |
| `cancellation-heavy` | replay | **2.45** | 408 | 1.85 – 2.54 | 1.62 – 2.62 | 11.3% | 1.98 / 2.50 / 2.46 |
| `mostly-resting` | core | **5.55** | 180 | 5.04 – 6.12 | 3.68 – 6.85 | 8.8% | 5.20 / 5.77 / 5.68 |
| `mostly-resting` | replay | **2.31** | 432 | 2.03 – 2.42 | 1.84 – 2.46 | 7.2% | 2.08 / 2.35 / 2.38 |
| `quote-refresh` | core | **10.21** | 98 | 8.93 – 10.64 | 6.44 – 10.87 | 7.6% | 10.16 / 10.33 / 10.18 |
| `quote-refresh` | replay | **2.35** | 425 | 1.81 – 2.44 | 1.05 – 2.48 | 14.8% | 2.13 / 2.39 / 2.39 |
| `pending-stops` | core | **3.50** | 286 | 3.30 – 3.61 | 2.94 – 3.71 | 4.0% | 3.50 / 3.50 / 3.49 |
| `pending-stops` | replay | **1.45** | 690 | 1.23 – 1.50 | 0.85 – 1.51 | 9.7% | 1.32 / 1.49 / 1.47 |
| `self-trade` | core | **5.39** | 186 | 5.02 – 5.54 | 4.11 – 5.62 | 4.8% | 5.47 / 5.29 / 5.37 |
| `self-trade` | replay | **1.68** | 594 | 1.61 – 1.75 | 1.32 – 1.79 | 4.6% | 1.63 / 1.73 / 1.71 |
| `immediate` | core | **5.45** | 183 | 5.17 – 5.60 | 4.50 – 5.77 | 4.3% | 5.48 / 5.42 / 5.47 |
| `immediate` | replay | **1.76** | 568 | 1.62 – 1.82 | 1.42 – 1.84 | 5.1% | 1.65 / 1.78 / 1.79 |
| `matching-heavy-request` | core | **5.61** | 178 | 5.03 – 5.81 | 4.42 – 5.97 | 6.4% | 5.54 / 5.69 / 5.64 |
| `matching-heavy-request` | replay | **1.77** | 566 | 1.63 – 1.88 | 1.38 – 1.92 | 6.6% | 1.67 / 1.84 / 1.85 |
| `matching-heavy-owned` | core | **5.50** | 182 | 4.96 – 5.75 | 3.95 – 5.80 | 6.0% | 5.55 / 5.49 / 5.48 |
| `matching-heavy-owned` | replay | **1.76** | 568 | 1.55 – 1.81 | 1.41 – 1.85 | 6.3% | 1.62 / 1.79 / 1.79 |

Every run's outcome matched the workload's prediction, including the behavior workloads' expirations, prevented self-trades, triggered stops, and pending stops. At this size the books end with:

- `small-book`: 173 orders, 10 levels
- `deep-book`: 99,555 orders, 2,000 levels
- `matching-heavy`: 10,147 orders, 194 levels
- `cancellation-heavy`: 49,692 orders, 500 levels
- `mostly-resting`: 86,874 orders, 100 levels
- `quote-refresh`: 1,805 orders, 101 levels
- `pending-stops`: 9,002 orders, 112 levels, 4,494 pending stops
- `self-trade`: 8,003 orders, 186 levels
- `immediate`: 11,081 orders, 86 levels
- `matching-heavy-request`: 10,147 orders, 194 levels
- `matching-heavy-owned`: 10,147 orders, 194 levels

- **The v1 workloads** run 1.5 to 1.8 times their first baseline's medians on `core` and 2.0 to 2.4 times on `replay`: the effect of the optimizations (whose interleaved measurements are in `OPTIMIZATION.md`; `mostly-resting`'s also includes the index's sizing hint, added after the first baseline), less the small cost the order behaviors left on v1 traffic ([`OPTIMIZATION.md`](OPTIMIZATION.md#the-v200-regression-check)). Sessions days apart are only a rough comparison; the paired runs are the measurement.
- **The request path** (`matching-heavy-request`, the same stream as `matching-heavy` through `submitOrder(OrderRequest)`) costs about 4% on `core`. Its `replay` row is slower than `matching-heavy`'s because its rows are twelve-column v2 rows, not because the book does more.
- **The owner probe** costs nothing measurable: `matching-heavy-owned` is within 2% of `-request`, as the paired measurement in `OPTIMIZATION.md` found.
- **`cancellation-heavy` core** is the noisiest row (CV 24%): its first session's median is 3.59 Mops/s against 5.83 and 5.32 in the next two. Treat its median as approximate.

### Cost of the replay layers

| Workload | core ns/command | replay ns/command | Difference |
|---|---:|---:|---:|
| `small-book` | 89 | 310 | 221 |
| `deep-book` | 243 | 498 | 255 |
| `matching-heavy` | 171 | 427 | 255 |
| `cancellation-heavy` | 193 | 408 | 215 |
| `mostly-resting` | 180 | 432 | 252 |
| `quote-refresh` | 98 | 425 | 327 |
| `pending-stops` | 286 | 690 | 404 |
| `self-trade` | 186 | 594 | 408 |
| `immediate` | 183 | 568 | 384 |
| `matching-heavy-request` | 178 | 566 | 387 |
| `matching-heavy-owned` | 182 | 568 | 386 |

The v1 workloads' replay layers now add about 215–255 ns per command, down from 580–650 in the first baseline, after the replay experiments of `OPTIMIZATION.md`. The behavior workloads' add 330–410 ns: their rows have twelve fields, and their commands make more events (expirations, preventions, triggered stops) to collect.

### Extended size (1,000,000 timed commands per run)

One session with 1 warm-up and 5 measured runs per workload and kind.

| Workload | Kind | Median Mops/s | ns/command at the median | Min – max Mops/s | Final book |
|---|---|---:|---:|---|---|
| `small-book` | core | 11.00 | 91 | 9.46 – 11.15 | 194 orders, 11 levels |
| `small-book` | replay | 3.35 | 299 | 3.34 – 3.37 |  |
| `deep-book` | core | 4.02 | 249 | 3.91 – 4.06 | 98,611 orders, 2,001 levels |
| `deep-book` | replay | 2.00 | 499 | 1.72 – 2.14 |  |
| `matching-heavy` | core | 5.48 | 183 | 5.43 – 5.50 | 11,201 orders, 198 levels |
| `matching-heavy` | replay | 2.37 | 422 | 2.34 – 2.40 |  |
| `cancellation-heavy` | core | 6.79 | 147 | 6.08 – 7.37 | 49,808 orders, 500 levels |
| `cancellation-heavy` | replay | 2.73 | 366 | 2.62 – 2.81 |  |
| `mostly-resting` | core | 4.57 | 219 | 4.45 – 4.69 | 862,963 orders, 100 levels |
| `mostly-resting` | replay | 2.20 | 454 | 1.98 – 2.24 |  |
| `quote-refresh` | core | 10.31 | 97 | 9.95 – 10.70 | 1,805 orders, 101 levels |
| `quote-refresh` | replay | 2.41 | 415 | 2.30 – 2.49 |  |
| `pending-stops` | core | 3.48 | 288 | 3.42 – 3.56 | 9,007 orders, 134 levels, 4,486 pending stops |
| `pending-stops` | replay | 1.50 | 666 | 1.37 – 1.52 |  |
| `self-trade` | core | 5.38 | 186 | 5.06 – 5.42 | 8,044 orders, 192 levels |
| `self-trade` | replay | 1.77 | 564 | 1.75 – 1.80 |  |
| `immediate` | core | 5.10 | 196 | 5.00 – 5.21 | 11,998 orders, 108 levels |
| `immediate` | replay | 1.81 | 554 | 1.76 – 1.82 |  |
| `matching-heavy-request` | core | 4.98 | 201 | 4.61 – 5.39 | 11,201 orders, 198 levels |
| `matching-heavy-request` | replay | 1.87 | 534 | 1.83 – 1.91 |  |
| `matching-heavy-owned` | core | 5.40 | 185 | 5.29 – 5.46 | 11,201 orders, 198 levels |
| `matching-heavy-owned` | replay | 1.77 | 564 | 1.73 – 1.81 |  |

Every steady-state workload keeps its book's shape at this size. `mostly-resting` grows as designed, to about 863,000 orders. `pending-stops` and `immediate` end with more levels than at the standard size (134 and 108, against 112 and 86) but stay in the range their bounds hold them to (`benchmarks/README.md`).

### How it was run

From `benchmarks/results/2026-10-29-order-behaviors/`, against the Release build of the commit above:

```bash
for s in 1 2 3; do
  order_book_benchmarks --size standard --kind both --repetitions 30 --warmup 1 --seed 42 \
    --csv standard-session$s.csv --json standard-session$s.json > standard-session$s.txt
done
order_book_benchmarks --size extended --kind both --repetitions 5 --warmup 1 --seed 42 \
  --csv extended.csv --json extended.json > extended.txt
python summarize.py
```

The Debug and Release test suites (77 tests each) passed on the same code before the runs. The same directory holds the paired before-and-after runs against v1.0.0's code and the experiments between them; its README lists them.

## History: the first baseline (2026-10-25)

The first recorded throughput of the order book, before the optimizations of [`OPTIMIZATION.md`](OPTIMIZATION.md) and the order behaviors of v2.0.0, taken with `order_book_benchmarks` on one desktop machine. It is a reference point for later changes to this code on this machine. It is not a measure of how the engine compares with any other system. How the harness works is in [`benchmarks/README.md`](../benchmarks/README.md). This page summarizes that and records what was run, what came out, and how far the numbers can be trusted.

| | |
|---|---|
| Date | 2026-10-25 |
| Code | commit `e22a9b2` on `main`, clean working tree |
| Workload generator | version 2, seed 42 |
| Build | MSVC 19.44.35228, Release (`/O2 /Ob2 /DNDEBUG`), no automatic invariant checks, no sanitizers |
| Machine | the reference machine in [`benchmarks/README.md`](../benchmarks/README.md#environment): AMD Ryzen 5 5600G, 32 GiB DDR4-3200, Windows 11 Pro 10.0.26200, High performance power plan |
| Raw results | [`benchmarks/results/2026-10-25-baseline/`](../benchmarks/results/2026-10-25-baseline/) |

### Results

![Strip plot of every measured standard-size run: 90 per workload and kind, with each row's median, CV, and range.](images/benchmark-baseline.svg)

#### Standard size (100,000 timed commands per run)

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

#### Cost of the replay layers

`replay` runs the same commands as `core`, but as in-memory CSV through the parser and the replay engine, including event collection, the final snapshot, and the formatted summary. The difference at the medians is the cost of those layers per command:

| Workload | core ns/command | replay ns/command | Difference |
|---|---:|---:|---:|
| `small-book` | 159 | 756 | 597 |
| `deep-book` | 446 | 1,101 | 655 |
| `matching-heavy` | 261 | 892 | 632 |
| `cancellation-heavy` | 332 | 944 | 613 |
| `mostly-resting` | 302 | 881 | 579 |

The replay layers add roughly 580–650 ns per command whatever the workload. That is between about 1.5 and 4 times the engine's own cost, so the replay path's throughput mostly measures CSV parsing and event collection, not the book.

#### Extended size (1,000,000 timed commands per run)

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

- **Workloads.** The five v1 workloads (`small-book`, `deep-book`, `matching-heavy`, `cancellation-heavy`, `mostly-resting`), each a preload plus a mix of limit-order submissions and cancellations with its own cancellation rate, price placement, and quantity sizes; and, in the v2.0.0 baseline, the six behavior workloads, which add requests, replaces, stops, and owners (`benchmarks/README.md`). Each is generated in full from its configuration and the seed before anything is timed, by a generator that follows its own model of the book and never touches `OrderBook`. The same generator version and seed give the same commands on every platform.
- **What is timed.** Each run builds a fresh book and applies the preload untimed. It then times every command with `std::chrono::steady_clock`, checks the outcome against the generator's prediction untimed, and destroys the book. `core` calls `submitOrder` and `cancelOrder` directly. `replay` parses the same commands as CSV text in memory through `ReplayEngine` with invariant validation off. Disk I/O is excluded from both.
- **Repetition.** One warm-up run per workload and kind, recorded in the raw files but left out of every statistic, then the measured runs. Every run starts from a fresh book.
- **Both baselines.** Standard size: three back-to-back sessions of 30 measured runs, pooled to 90 per row, so the result does not rest on one session. Extended size: one session of 5 measured runs, as a check that results hold at a larger scale.
- **Statistics.** Medians rather than means, since a run disturbed by the machine is almost always slower, and the median is not pulled down by a few such runs. Each baseline's `summarize.py` ([2026-10-29](../benchmarks/results/2026-10-29-order-behaviors/summarize.py), [2026-10-25](../benchmarks/results/2026-10-25-baseline/summarize.py)) recomputes its tables from the raw CSV files.

The commands that produced the first baseline's raw results, run from `benchmarks/results/2026-10-25-baseline/` against the Release build of its commit:

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
- **Synthetic workloads.** The workloads are generated mixes chosen to stress particular parts of the book: overhead, a large working set, matching, the order index, growth, and in v2.0.0 replaces, stops, self-trade prevention, and immediate orders. They are not recordings of real market activity. They use a fresh ID for every order and a single symbol; the v1 workloads use limit orders only. Real order flow has bursts, correlated cancellations, and a very different price and size distribution.
- **Fresh books and allocator state.** Every run starts from a newly preloaded book, in a process whose heap has been through earlier runs. The cost of the preload is excluded, and the allocator's state carries over between runs, which may account for part of the drift in the large-book workloads.
- **`mostly-resting` grows by design**, so its numbers depend on the run length (see [Extended size](#extended-size-1000000-timed-commands-per-run-1)).
- **Generator version.** Each baseline holds for its own workload generator version (2 for the first, 3 for v2.0.0) and seed 42 only; version 3 left the five v1 workloads' commands unchanged. A change to the generator bumps its version and makes earlier results incomparable.

## What these numbers do not show

They are not a comparison with any production exchange, trading venue, or commercial matching engine, and should not be presented as one. Such systems do different work (many symbols, order types, risk checks, networking, persistence, replication, regulatory recording) on different hardware under different load, and their published figures are measured in other ways. Nothing on this page supports a claim of being "low latency", "microsecond", or "exchange grade", or any claim about tail latency. What the numbers do support is narrower: on this machine and build, with these synthetic workloads, one book processes a few million commands per second on one thread, and the engine's shards add to that on each physical core given to them, up to the six measured. They also give a reference for measuring later changes to this code in the same environment.
