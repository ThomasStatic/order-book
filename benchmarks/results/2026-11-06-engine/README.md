# v2.1.0's engine: the recorded Windows set and the gate (spec #58, V2b, #97)

This was recorded by `benchmarks/record_baseline.py` in one unattended session on the reference machine (`machine.txt` in `../2026-11-05-latency-v1/`):
- **When.** 2026-11-06, from 01:32 to 13:54. That's 754 steps, and none failed (`session.log`).
- **The re-checks.** Four comparisons ran again at 24 pairs afterwards, from 14:04 to 14:24.
- **The build.** The Release build of `2affa3b`, MSVC with the Visual Studio 17 2022 generator. Threaded cells are pinned at high priority.
- **The tables.** `summary.md` holds every table, as `summarize.py` (a copy of `benchmarks/summarize_baseline.py`) recomputes them from the files:

  ```
  python benchmarks/results/2026-11-06-engine/summarize.py --out benchmarks/results/2026-11-06-engine
  ```

The session's command is in `record_baseline.py`'s docstring:
- `--base` is tag v2.0.0's `order_book_benchmarks`.
- `--before-latency` and `--before-component` are commit `17a8013`'s latency and component programs, which hold v2.0.0's library.
- Each was built in a worktree with `-DORDER_BOOK_BUILD_BENCHMARKS=ON`.

## What is here

| Directory | What |
|---|---|
| `pairs/aa-*` | Each threaded kind's A/A floor, the same binary on both sides, run before anything else |
| `pairs/gate-standard`, `pairs/gate-extended` | `core` and `replay` against tag v2.0.0 on all eleven workloads, 16 ABBA pairs |
| `pairs/*` | The other paired comparisons, named `base-exp-workload`; a `-24` suffix is a 24-pair re-check |
| `runs/s1` to `runs/s3` | Three sessions of every cell, each `order_book_benchmarks`' CSV and JSON |
| `latency/` | The latency rows, each with its report, run record, and histogram counts; `zero-heap-*` are the before-and-after |
| `noise/` | The latency harness's noise floor before and after each session |
| `components/` | The component benchmarks, with the controls on each layout's cores |
| `logs/` | Each step's console output |
| `gate-bisect/` | The gate exception's bisect; see below |

## The gate

No workload may be more than 3% slower than tag v2.0.0, or each exception must be explained and accepted. At both sizes, ten of the eleven `core` rows are faster or within 3%. The one exception, accepted:

| Size | `quote-refresh`, now / v2.0.0 | 95% CI |
|---|---:|---|
| standard | 0.963 | 0.952 - 0.967 |
| extended | 0.964 | 0.961 - 0.973 |

**The explanation.** `gate-bisect/` reads `quote-refresh` against v2.0.0 at standard size, at each commit that touched the book's hot path and at the commits after them.

| Commits | Ratio |
|---|---|
| `92a1d24` to `522461c`, seven builds | 0.979 to 0.987 |
| `68601c6` (V2a's ladder) | 0.961 |
| `HEAD` | 0.959 |

The step at `68601c6` isn't the engine:
- **The library.** Every object of `order_book` disassembles to the same instructions on both sides of the step.
- **The timed code.** The benchmark's `runCore` and `execute` are the same instructions and calls too. `gate-bisect/*.s` holds them with addresses stripped.

`68601c6` added code elsewhere in the executable, so the same code links at different addresses. `quote-refresh`, the cheapest command per operation, shows that most.

What's real is the 1.3 to 2.1% left since L4's result reuse, which L5 (#65) flagged at 0.974. It's tracked in #107. The two other dips in the bisect, `d774b61` and `b58bc49`, are real code changes their own tickets fixed.

`replay` is reported, not gated. Its `quote-refresh` reads 0.941 and 0.933, and `small-book` 0.955 and 0.951. `replay` parses CSV in the timed region, so it carries the same book plus the parser.

## The A/A floors and the re-checks

| Kind | Floor |
|---|---:|
| `shard` | ±1.9% |
| `queue` | ±1.4% |
| `gateway` | ±0.4% |
| `engine` | ±1.4% |

Four comparisons reached within their kind's floor of 1, so they ran again at 24 pairs:

| Pair | Ratio | 95% CI |
|---|---:|---|
| `gateway-engine-deep-book-24` | 0.993 | 0.988 - 0.997 |
| `gateway-engine-immediate-24` | 1.005 | 0.993 - 1.026 |
| `gateway-engine-quote-refresh-24` | 1.013 | 1.002 - 1.017 |
| `process-machine-pending-stops-24` | 1.014 | 1.001 - 1.034 |

None differs by more than 1.4%. For `gateway` against `engine`, both sides run unpinned, since the engine kind can't place the shipped engine's threads.

## Zero heap

`matching_path_allocation_tests` passes. The before-and-after compares command service time with v2.0.0's library, four sessions pinned, in ABBA order:
- **p50:** 0.86 to 1.00 of before, never slower.
- **p99:** 0.91 to 0.98 of before. `mostly-resting` is the exception, at 0.44, from 1,058 ns to 466 ns.

No difference at p50 is a valid result.

## Reversals and follow-ups

- **#108 (new).** A roll-forward worker nearly halves `queue` throughput at the `long` size: 0.531 to 0.568 at N = 1. D4 (#91) says it costs a matching thread nothing. In this layout the worker shares the log writer's logical processor, which is the matching thread's sibling, and the lowest thread CPU share falls from about 0.95 to about 0.45. The shipped engine doesn't pin its roll threads.
- **#107 (new).** The real part of the gate exception, above.
- **#101 stays open.** Pinned, `shard` / `core` reads 0.785 on `deep-book` and 0.831 on `quote-refresh`. Derived, `shard` reads 0.82 and 0.80 of v2.0.0 there, and 0.98 to 1.10 on the other three.
- **#102 closed.** Pinned, `queue` / `core` reads 0.755 to 0.777 on four workloads, within #29's 0.71 to 0.94, and 0.872 on `pending-stops`, against #29's 0.94. The earlier gap was mostly that the read wasn't pinned.
- **#103 closed.** Pinned, window 1 reads 101.8k/s, 9.8 µs a command, against #27's 130k/s for a bare echo. The extra 2.2 µs is decoding, two ring hand-offs (p50 about 0.44 µs each, `latency/hand-off-ladder`), matching, and encoding.

## Notes for the write-up (V3, #98)

- **No scaling claim goes past 6 physical cores.** `shard` at N = 6 against N = 1 reads 3.46 to 5.26. Cells past six threads place the rest on siblings and are named `siblings`.
- **`machine` above `process`.** `process-machine` reads above 1 on `immediate` (1.081) and `matching-heavy-request` (1.060). In `queue` the matching thread never waits for durability, so `machine`'s flushes cost the writer, not the matching thread, and fewer, larger writes may disturb the matching thread's core less. It isn't a reason any ticket gave, so it gets no issue, but it's worth a sentence where durability's cost is quoted.
- **The latency rows** carry their own quotable verdicts and noise marks in `summary.md`.
- **Session length.** The session took 12 h 22 min, not the spec's 4 h. Most of the excess is the machine-durability latency ladders, which took 30 to 42 minutes a row.
