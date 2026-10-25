# Order Book

A correctness-first C++ limit-order-book and matching-engine project.

## Repository layout

- `include/` — public headers for the reusable library
- `src/` — implementation for the reusable library
- `apps/` — thin CLI entry points
- `tests/` — lightweight infrastructure tests
- `benchmarks/` — optional benchmark placeholders
- `docs/` — design notes

## Build and test

Use out-of-source builds.

### Debug build

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

### Release build

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

### Run the CLI

`order_book_cli` replays a CSV command file ([`docs/replay-format.md`](docs/replay-format.md)) on an empty book and prints the replay statistics. It never prompts; everything is set by arguments, so it scripts cleanly.

```bash
./build/debug/order_book_cli --help                                          # usage, options, exit statuses
./build/debug/order_book_cli replay tests/replay/valid/basic.csv             # statistics only
./build/debug/order_book_cli replay tests/replay/valid/basic.csv --trades    # each trade, then the statistics
./build/debug/order_book_cli replay tests/replay/valid/basic.csv --book      # the statistics, then the final book
./build/debug/order_book_cli replay tests/replay/valid/price_time_priority.csv --trades --book
./build/debug/order_book_cli replay tests/replay/valid/basic.csv --events --validate per-command
./build/debug/order_book_cli demo                                            # a scripted demo session
```

| Option | Effect |
|---|---|
| `--trades` | print each trade, tagged with the line of the command that made it, before the statistics |
| `--events` | print every event instead (acceptances, rejections, trades, cancels) |
| `--book` | print the final book after the statistics, every level, best price first |
| `--validate none\|final\|per-command` | how often to check the book's invariants (default `final`) |
| `--` | the next argument is the file, even if it starts with `-` |
| `-h`, `--help` | print help to stdout and exit 0 |

Options go before or after the file. For example, `replay tests/replay/valid/basic.csv --trades` prints:

```text
line 4: TradeExecuted incoming=1003 resting=1002 price=105 qty=20

commands processed: 4
add commands: 3
adds accepted: 3
adds rejected: 0
cancel commands: 1
cancels accepted: 1
cancels rejected: 0
trades executed: 1
volume traded: 20
resting orders: 1
```

Exit statuses: `0` replayed to the end, even if the book rejected some commands (or help was printed); `1` input error (the file can't be opened or read, or is malformed; what ran before the error is still printed, and the diagnostic goes to stderr); `2` usage error (stderr names the problem and shows the usage); `3` engine failure (the book threw or failed an invariant check). Output is byte-identical on every platform, with LF line endings.

### Enable benchmarks

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DORDER_BOOK_BUILD_BENCHMARKS=ON
cmake --build build/debug --target order_book_benchmarks
./build/debug/order_book_benchmarks
```

### Enable sanitizers

Sanitizer builds should normally use Debug or RelWithDebInfo.

```bash
cmake -S . -B build/sanitizers -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DORDER_BOOK_ENABLE_ASAN=ON -DORDER_BOOK_ENABLE_UBSAN=ON
cmake --build build/sanitizers
```

UBSan is built with `-fno-sanitize-recover`, so any finding fails the test that hit it.

With MSVC only AddressSanitizer is available (`ORDER_BOOK_ENABLE_UBSAN` is ignored with a warning). The ASan runtime DLL is copied next to each executable, so tests run from any shell:

```bash
cmake -S . -B build/asan -G "Visual Studio 17 2022" -DORDER_BOOK_ENABLE_ASAN=ON
cmake --build build/asan --config Debug
ctest --test-dir build/asan -C Debug --output-on-failure
```

### Invariant checks

`OrderBook::checkInvariants()` validates the whole book (levels, aggregates, FIFO order, the order index, and an uncrossed top of book) and reports the first violation it finds. By default (`ORDER_BOOK_INVARIANT_CHECKS=AUTO`), Debug builds also run it after every public command that can change the book and abort on a violation. `ON` keeps that in optimized builds too, and `OFF` drops it from Debug builds; `checkInvariants()` stays callable either way.

```bash
cmake -S . -B build/invariants -G Ninja -DCMAKE_BUILD_TYPE=Release -DORDER_BOOK_INVARIANT_CHECKS=ON
cmake --build build/invariants
ctest --test-dir build/invariants --output-on-failure
```

### Randomized command tests

`randomized_tests` generates long, deterministic streams of submissions and cancellations, valid and intentionally invalid, from a seed and a workload profile. It runs each stream against the book beside an independent reference model, and after every command checks the book's invariants, the exact result and events, and every level and FIFO queue. This is seeded pseudorandom testing, not coverage-guided fuzzing. CTest runs the committed seeds (`randomized_seeds`) and replays the saved streams in `tests/randomized/regressions/` (`randomized_regressions`).

```bash
./build/release/randomized_tests                                   # the committed seeds
./build/release/randomized_tests --list-profiles
./build/release/randomized_tests --seed 12345 --profile balanced --commands 20000
./build/release/randomized_tests --explore 50                      # 50 fresh seeds, each printed before it runs
./build/release/randomized_tests --replay path/to/stream.txt      # exact replay of a saved stream
./build/release/randomized_tests --seed 12345 --profile queue --write stream.txt   # save without running
```

On the first failure it stops and prints the seed, profile and generator version, command index, command, the invariant or expectation broken, the book and model state, and the commands leading up to it. It then saves the executed prefix and a minimized stream to `--out-dir` (default `randomized-failures`) and prints the lines that replay them. To keep a failure as a permanent regression, copy the minimized stream into `tests/randomized/regressions/`.

Use a Release build (or `ORDER_BOOK_INVARIANT_CHECKS=OFF`) for exploration. With automatic invariant checks on, the engine aborts at its own boundary before the harness can report. The harness then prints only the seed, the command, and a line to reproduce the failure.

### Continuous integration

`.github/workflows/ci.yml` builds and tests every push and pull request, with warnings as errors and benchmarks enabled: GCC and Clang Debug and Release, GCC and Clang ASan+UBSan Debug, Clang ASan+UBSan RelWithDebInfo, and MSVC Debug, Release, and ASan Debug and Release. A new push to a pull request cancels that pull request's run still in progress; push runs are never cancelled.

Every test runs in at least one job, but some skip by design in others: the `invariant_boundary_*` tests in Release and RelWithDebInfo, which have no automatic invariant checks, and `allocation_failure_tests` in MSVC Debug, whose checked iterators allocate inside `noexcept` members.

### Clean build from scratch

```bash
rm -rf build/debug build/release build/sanitizers
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

## Notes

- The reusable library target is `order_book`.
- The CLI target is `order_book_cli`.
- The smoke-test target is `order_book_tests`.
- Benchmarks are only built when `ORDER_BOOK_BUILD_BENCHMARKS` is enabled.
- The current implementation assumes a fixed tick size of $0.01 for simplicity.
