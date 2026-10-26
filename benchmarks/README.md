# Benchmarks

`order_book_benchmarks` measures how fast the book executes commands. Each workload is generated from its configuration and a seed. Its commands are then timed on a freshly preloaded book, and every run is repeated. Everything is recorded, and results can be written as CSV or JSON.

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DORDER_BOOK_BUILD_BENCHMARKS=ON
cmake --build build/release --target order_book_benchmarks
./build/release/order_book_benchmarks                       # every workload, standard size, core engine
./build/release/order_book_benchmarks --list
./build/release/order_book_benchmarks --workload deep-book --size extended --repetitions 10
./build/release/order_book_benchmarks --kind both --csv results.csv --json results.json
```

With the Visual Studio generator, build and run the Release configuration: `cmake --build build --config Release --target order_book_benchmarks`, then `build/Release/order_book_benchmarks.exe`.

| Option | Effect |
|---|---|
| `--workload NAME` | run this built-in workload; repeat for several (default: all) |
| `--size smoke\|standard\|extended` | 10,000, 100,000 (default), or 1,000,000 timed commands per run |
| `--commands N` | exactly N timed commands, instead of a size (recorded as size `custom`) |
| `--kind core\|replay\|both` | which path to time (default `core`, see below) |
| `--repetitions N` | measured runs per workload and kind (default 5) |
| `--warmup N` | runs before those, recorded but left out of summaries (default 1) |
| `--seed N` | generator seed (default 42) |
| `--csv FILE`, `--json FILE` | write the results (see below) |
| `--allow-unoptimized` | run in a build that is not optimized; its numbers are not meaningful |

Exit statuses: `0` every run completed; `1` a run did not do what its workload predicted, or an output file could not be written; `2` usage error; `3` the build is not optimized and `--allow-unoptimized` was not given.

## Release builds only

The program refuses to run unless `NDEBUG` is defined, automatic invariant checks are off, and no sanitizer is in use: neither AddressSanitizer (detected by the compiler) nor UBSan (from `ORDER_BOOK_ENABLE_UBSAN`, since GCC gives no sign of it). The default `ORDER_BOOK_INVARIANT_CHECKS=AUTO` checks the whole book after every command in Debug builds, which turns each O(1) or O(log n) command into an O(n) one. Debug builds also keep assertions and MSVC's checked iterators. Neither kind of number says anything about the engine. `--allow-unoptimized` runs anyway (CTest uses it for a smoke run in every configuration), and the build is recorded with the results either way.

## What is timed

Generation and execution are separate. The generator decides what happens. The book decides how fast that runs. Mixing their costs would make a measurement hard to interpret, so every workload is generated in full before anything is timed. Each run then:

1. builds a fresh `OrderBook` and applies the workload's preload (untimed);
2. reads the clock (`std::chrono::steady_clock`), runs every timed command, and reads the clock again;
3. checks the book's outcome (trades, volume, final resting orders and levels, no rejections) against the outcome the generator predicted, and fails the session if they differ (untimed);
4. destroys the book (untimed).

A measurement is one run: commands, elapsed nanoseconds, operations per second (commands / elapsed seconds), and nanoseconds per command. Warm-up runs come first. They are recorded, but the summaries leave them out. A first run can be slower for reasons unrelated to the engine, such as cold caches, page faults on fresh memory, and CPU frequency ramping up. Every repetition starts from a fresh book, so no run inherits another's state.

The two kinds differ only in what sits between the commands and the book:

- **`core`** times the engine alone. Each command is an `Order` passed straight to `submitOrder`, or an ID passed to `cancelOrder`. Inside the timed loop, only each result's trades, volume, and rejections are tallied. That keeps the results from being optimized away and is needed for the outcome check. No CSV is involved.
- **`replay`** times the end-to-end path of [`docs/replay-format.md`](../docs/replay-format.md): CSV parsing, the replay engine's dispatch and event collection, the final snapshot, and the formatted summary. The same commands are first rendered as CSV text in memory (untimed). Reading a file from disk is left out on purpose, since it would mostly measure the OS file cache. Invariant validation is `none`, so this path does no checking work that `core` skips.

The difference between the two kinds is the cost of the replay layers.

## Workloads

| Workload | Initial orders | Target levels | Cancellation rate | Prices (uniform / concentrated / marketable) | Quantities (small / medium / large) | Objective |
|---|---|---|---|---|---|---|
| `small-book` | 100 | 10 | 35% | 35 / 40 / 25 | 100 / 0 / 0 | baseline overhead |
| `deep-book` | 100,000 | 2,000 | 35% | 50 / 17 / 33 | 40 / 50 / 10 | large working set |
| `matching-heavy` | 10,000 | 200 | 8% | 10 / 32 / 58 | 20 / 60 / 20 | matching efficiency |
| `cancellation-heavy` | 50,000 | 500 | 50% | 50 / 50 / 0 | 50 / 50 / 0 | order index and removal |
| `mostly-resting` | 1,000 | 100 | 5% | 80 / 18 / 2 | 50 / 40 / 10 | insertion and book growth |

- **Preload.** The initial orders are spread round-robin over the target levels, alternating bid and ask, one tick apart either side of a middle price of 100,000 ticks. None of them trade.
- **Cancellation rate** is r_cancel = cancellation commands / total timed commands. A cancellation always names a resting order, picked uniformly. One drawn while nothing rests becomes a submission instead, so the realized rate, reported with every run, can fall slightly below the configured one.
- **Prices** are integer ticks (`Price`), drawn relative to the book just before the command. *Uniform* is anywhere in the order's own side's band of levels. *Concentrated* is 0-3 ticks behind its own side's best price. *Marketable* is at the opposing best price (matching-heavy goes up to 2 ticks through it), so it trades. Only marketable orders cross. The others are held one tick behind the opposing best, so the book keeps about its target number of levels.
- **Quantities**: small 1-10, medium 11-100, large 101-1000.
- **Steady state.** No command depends on the book's size, so an unbalanced mix drifts steadily and a balanced one wanders like a random walk. The mixes are tuned to keep each book's shape from 10k to 1M commands. Small-book also has a cap of 200 resting orders: a submission drawn at the cap is priced marketable instead, which leaves the cancellation rate unchanged. A book cannot sustain a cancellation rate above 50%, since every cancellation removes an order that a submission had to add. Cancellation-heavy sits exactly at that balance: none of its submissions trade, so each one rests an order and each cancellation removes one. Deep-book relies on matching for part of its removal, so it can stay clearly lower, at 35%. Mostly-resting is meant to grow, and does: from 1,000 to about 87,000 orders at the standard size.

### Determinism

The same configuration, size, seed, and `workloadGeneratorVersion` (in `workload.hpp`) give exactly the same commands on every platform. The generator draws from `std::mt19937_64`'s raw output with its own unbiased reduction, never a standard distribution, whose results differ between standard libraries. Each draw is a statement of its own, since argument evaluation order differs between compilers. It also never touches `OrderBook`. It follows its own small model of the book (`WorkloadModel`, in `workload.cpp`), so a workload does not depend on the engine it measures, and the model's prediction checks each run independently. Order IDs are fresh for every order and never reused.

`benchmark_harness_tests` pins a fingerprint of each built-in workload at the smoke size. Any change to what is generated must bump `workloadGeneratorVersion` and re-pin. Results are only comparable under the same generator version, seed, and size. Each result records all three, plus the workload's fingerprint (JSON).

## Results

Every run is printed as it completes, followed by a summary per workload and kind: measured repetitions, median, minimum, and maximum operations per second, nanoseconds per command at the median, and the coefficient of variation (sample standard deviation / mean). A large coefficient of variation means the machine was noisy. Rerun with more repetitions, or on a quieter machine, before comparing.

`--csv` writes one line per run, warm-ups included (the `warmup` column marks them):

```text
workload,kind,size,seed,generator_version,build_config,optimized,repetition,warmup,commands,elapsed_ns,ops_per_second,ns_per_command,submits,cancels,cancellation_rate,rejections,trades,volume,final_active_orders,final_price_levels
```

`--json` writes the whole session: generator version, seed, size, warm-ups and repetitions, start time (Unix seconds), the build (configuration, compiler, `NDEBUG`, invariant checks, AddressSanitizer, UBSan), each workload's configuration, fingerprint, expected outcome, and untimed generation time, the summaries, and every raw measurement. Both files are written with LF line endings, and numbers are formatted without regard to locale.

## Environment

A result means little without the machine and build that produced it. Compare results only from the same environment, and publish every number with the record below. The program records part of it with every result. The rest has to be written down by hand.

| Item | Recorded by the program | Reference machine |
|---|---|---|
| CPU model | no | AMD Ryzen 5 5600G (Zen 3), 6 cores / 12 threads, 3.9 GHz base, up to 4.4 GHz boost, 512 KiB L2 per core, 16 MiB shared L3 |
| Memory | no | 32 GiB DDR4-3200 (two 16 GiB modules) |
| Operating system | no | Windows 11 Pro, 64-bit, version 10.0.26200 |
| Compiler and version | yes (JSON `build.compiler`, as `_MSC_FULL_VER`, `__clang_version__`, or `__VERSION__`) | MSVC 19.44.35228 (Visual Studio 2022 Build Tools, toolset 14.44.35207), x64 host and target; CMake 4.4.3, Visual Studio 17 2022 generator |
| Compiler flags | no | see below |
| Build type | yes (`build_config`, `optimized`; JSON also `ndebug`, `invariant_checks`, sanitizers) | Release; `ORDER_BOOK_INVARIANT_CHECKS=AUTO`, so off; no sanitizers |
| Number of commands | yes (`size`, `commands`) | standard: 100,000 timed commands per run, after each workload's untimed preload |
| Workload distribution | yes (workload name, `generator_version`, `seed`; JSON also each workload's configuration and fingerprint) | the five built-in workloads ([Workloads](#workloads)), generator version from `workload.hpp`, seed 42 |
| Number of repetitions | yes (`repetition`, `warmup`; JSON also the totals) | 1 warm-up, then 5 measured runs per workload and kind |
| Machine conditions | no | see below |

### Compiler flags

The project adds no optimization flags of its own: Release uses CMake's defaults plus the warning level and the language standard. On the reference machine, every target is compiled with:

```text
/DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG /W4 /std:c++20 /MD
```

and linked with `/INCREMENTAL:NO`. There is no link-time code generation (`/GL`, `/LTCG`), no profile-guided optimization, and no `/arch` option, so the code targets baseline x64 (SSE2). With GCC or Clang, CMake's Release default is `-O3 -DNDEBUG`, plus `-std=c++20` and the warnings in `CMakeLists.txt`, again without `-march`. A build with other flags (a `-march=native`, LTO, a different `CMAKE_CXX_FLAGS_RELEASE`) is a different environment. Record its flags in full: CMake shows them in `CMakeCache.txt`, and a Ninja or Makefile build's `compile_commands.json` holds each file's exact command line.

### Machine conditions

The benchmark runs on one thread. It does not pin itself to a core or raise its priority, so the scheduler, frequency scaling, and anything else running on the machine all reach the timings. On the reference machine:

- it is a desktop on mains power, with the Windows power plan set to High performance;
- `std::chrono::steady_clock` is MSVC's `QueryPerformanceCounter` clock at 10 MHz, a 100 ns tick, far finer than any run at the smoke size or above;
- results are taken from a terminal, not under a debugger or profiler, with other programs closed.

Record any departure from these, and anything else that could move the numbers: running on battery or a power-saving plan, a laptop's thermal limits, other heavy work on the machine, a virtual machine or container, or changed firmware settings such as boost or SMT. The coefficient of variation in each summary is the check that the machine was quiet: if it is large, the conditions were not steady enough for the results to be compared.

## Code

- `workload.hpp`, `workload.cpp`: sizes, workload configurations, the built-in workloads, the model, and the generator.
- `benchmark_runner.hpp`, `benchmark_runner.cpp`: timed runs of both kinds, repetitions, outcome checks, and rendering commands as replay CSV.
- `benchmark_report.hpp`, `benchmark_report.cpp`: build detection, summaries, and the CSV, JSON, and table formats.
- `order_book_benchmark.cpp`: the program.

These are built into the `order_book_benchmarking` library (linking `order_book_replay`) when `ORDER_BOOK_BUILD_BENCHMARKS` is on. `tests/benchmark_harness_test.cpp` (`benchmark_harness_tests`) tests the library. `benchmark_cli_smoke` runs the program once on a tiny workload, writing both files.

Recorded results live in `results/`, one directory per session set, kept byte for byte as the program wrote them. The baseline is `results/2026-10-25-baseline/`, written up in [`docs/BENCHMARKS.md`](../docs/BENCHMARKS.md); its `summarize.py` recomputes that page's tables from the raw CSV files.
