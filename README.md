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

```bash
./build/debug/order_book_cli                                         # a scripted demo session
./build/debug/order_book_cli replay tests/replay/valid/basic.csv     # replay a CSV file
```

`replay` prints each event with its line, then a deterministic summary: command and execution counts, and the final book, best price first. See [`docs/replay-format.md`](docs/replay-format.md) for the file format, the `--validate none|final|per-command` option, and exit statuses.

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
