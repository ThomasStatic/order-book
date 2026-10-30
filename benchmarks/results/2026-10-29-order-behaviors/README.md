# v2.0.0: regression check and baseline (spec #40, P3)

Written up in [`docs/OPTIMIZATION.md`](../../../docs/OPTIMIZATION.md#the-v200-regression-check) (the regression check) and [`docs/BENCHMARKS.md`](../../../docs/BENCHMARKS.md) (the baseline). Release builds, `/O2 /Ob2 /DNDEBUG`, Visual Studio 17 2022 generator, MSVC 19.44, on the reference machine of `benchmarks/README.md`; seed 42.

## The baseline

`standard-session{1,2,3}.{csv,json,txt}` and `extended.{csv,json,txt}`, exactly as `order_book_benchmarks` wrote them, from the Release build of `f7db5b5`, generator version 3. `summarize.py` recomputes `BENCHMARKS.md`'s v2.0.0 tables from them. The commands are on that page.

## Paired runs

Each directory holds `summary.md`, as `benchmarks/compare_ab.py` wrote it (its binary paths are copies of each build, in a scratch directory), and `runs.csv`: every invocation's CSV rows concatenated, with two leading columns, `pair` (its number) and `binary` (`base` or `exp`). Only the first 21 columns are kept, the ones both builds write; v1.0.0's binary writes no others. Each invocation is 1 warm-up and 5 measured runs, in ABBA order.

| Directory | Base | Experiment | Size, kinds | Pairs |
|---|---|---|---|---:|
| `paired-standard` | `f22a8f4` (v1.0.0's code) | `f7db5b5` (v2.0.0) | standard, both | 16 |
| `paired-extended` | `f22a8f4` | `f7db5b5` | extended, both | 8 |
| `before-recovery/paired-standard` | `f22a8f4` | `a4809ff` (before experiments A and B) | standard, both | 16 |
| `before-recovery/paired-extended` | `f22a8f4` | `a4809ff` | extended, both | 8 |
| `experiments/a-recorder-inline` | `a4809ff` | A: `ResultRecorder`'s per-trade path inline, results' vectors taken by rvalue reference | standard, `core`, `small-book` and `matching-heavy` | 16 |
| `experiments/a-vs-v1` | `f22a8f4` | A | standard, both, the same two workloads | 8 |
| `experiments/b-reserve-events` | A | B: A plus `ReplayEngine::run` reserving one event per record (`f7db5b5`) | standard, `replay`, the same two | 12 |
| `experiments/b-vs-v1` | `f22a8f4` | B | standard, both, all five | 8 |
| `experiments/c-compact-record-reverted` | B | B plus a packed `AddCommand` (reverted) | standard, `replay`, the same two | 12 |

A first baseline was also taken on `a4809ff`, before experiments A and B. It was discarded, unused, once they were kept, and the baseline here was taken again on `f7db5b5`.
