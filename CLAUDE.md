# CLAUDE.md

A correctness-first C++20 limit-order-book and matching-engine library: the static library `order_book`, the non-interactive CLI `order_book_cli` (`replay`, `convert`, `demo`; see `--help`), the engine executable `order_book_engine`, and optional benchmarks. Released as `v2.0.0`. README's "Public API" section lists which headers are the API, and a change to those is breaking.

All code is in namespace `order_book`, with headers in `include/order_book/` and implementations in `src/`. Domain terms are in `GLOSSARY.md` and decisions in `docs/adr/`.

## Internals

Read the matching note in `docs/internals/` before changing that area, and update it in the same commit as the change:

- `engine.md`: read before changing `src/` or `include/`. Types, sequence numbers, `PriceLevel`, `OrderBook`, the index, the node pool, matching, results, events, invariants, errors.
- `order-behaviors.md`: `OrderRequest`, expiry, IOC, FOK, market, post-only, self-trade prevention, pending stops, cascades, cancel-replace.
- `testing.md`: CTest names, CMake options, what each suite covers, consistency helpers, allocation-failure tests.
- `randomized-tests.md`: the command generator, profiles, `ReferenceBook`, `CommandReplayer`, saved regressions, `generatorVersion`.
- `replay.md`: the CSV parser, replay engine, samples, and expected CLI output.
- `benchmarks.md`: workloads, `workloadGeneratorVersion`, recorded baselines, what results may claim, profiling.
- `latency.md`: the latency harness: clock, histogram, drivers, run record, report.
- `runtime.md`: `order_book_runtime`, and `EngineSettings`: the instrument list, placement, `InstrumentDirectory`, `BookSet`, `Shard`, the rings, `MatchingLoop`, `LogStorage`, log files and `Housekeeping`, `LogWriter`, the durable position.
- `network.md`: `order_book_network`: `FrameReader`, the frame encoders, `checkEntry`, `SocketSet`, `GatewayThread` and the hold, `TcpSocket`, `PollSocketSet`, `runEngine`, `StopSource`.

## Build and test

Out-of-source builds under `build/`. README documents Ninja, but on this machine `build/` is configured with the **Visual Studio 17 2022** multi-config generator, so pass the config explicitly:

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Debug -R price_level_tests --output-on-failure
```

Warnings are high (`/W4`, or `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`), so watch for sign/narrowing conversions.

CI (`.github/workflows/ci.yml`) runs on every push and pull request with `ORDER_BOOK_WARNINGS_AS_ERRORS=ON`: on Linux, GCC and Clang Debug/Release, GCC and Clang ASan+UBSan Debug, and Clang ASan+UBSan RelWithDebInfo; on Windows, MSVC Debug/Release and ASan Debug/Release. ThreadSanitizer (Clang RelWithDebInfo, GCC Debug) and Linux Arm Clang Release run the tests labelled `threaded`. A new push to a pull request cancels that pull request's run still in progress; push runs are never cancelled. Code that builds cleanly with MSVC can still fail there on a GCC/Clang warning.

## Writing tests

- No test framework: each `tests/*.cpp` is its own executable with a `main()` using local `expect(cond, msg)` / `expectThrows(fn, msg)` helpers that print `FAILED:` and `exit(EXIT_FAILURE)`.
- Each test file needs its own `add_executable` + `order_book_enable_warnings` + `order_book_enable_sanitizers` + `add_test` block in `CMakeLists.txt`. New `.cpp` files of the `order_book` library must be added to its target source list (the replay parser and engine are deliberately in their own library, `order_book_replay`).
- Tests reach private members only through the `OrderBookTestAccess` friend (declared in `OrderBook`, `PriceLevel`, and `Order`, and defined separately in each test executable that needs it), so keep those definitions in mind when renaming private members. Don't `#define private public`: redefining a keyword around standard headers is undefined behavior, Clang rejects it under `-Werror` (`-Wkeyword-macro`), and on MSVC it fails to link for member functions, since access level is part of the mangled name.
