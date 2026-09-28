# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A correctness-first C++20 limit-order-book and matching-engine library (`order_book` static lib), with a placeholder CLI (`order_book_cli`) and optional benchmarks. Matching logic is not yet implemented; the storage layer (price levels, order index, snapshots) is.

## Build and test

Out-of-source builds under `build/`. README documents Ninja:

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

On this machine `build/` is currently configured with the **Visual Studio 17 2022** multi-config generator, so pass the config explicitly:

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Run a single test by CTest name with `-R` (names: `order_book_smoke`, `order_tests`, `result_type_tests`, `price_level_tests`), e.g. `ctest --test-dir build -C Debug -R price_level_tests --output-on-failure`, or build and run one target directly (`cmake --build build --config Debug --target price_level_tests`).

CMake options: `ORDER_BOOK_BUILD_BENCHMARKS`, `ORDER_BOOK_ENABLE_ASAN`, `ORDER_BOOK_ENABLE_UBSAN` (GCC/Clang only; ignored with a warning on MSVC), `ORDER_BOOK_WARNINGS_AS_ERRORS`. Warnings are high (`/W4`, or `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`), so watch for sign/narrowing conversions.

## Tests

- No test framework: each `tests/*.cpp` is its own executable with a `main()` using local `expect(cond, msg)` / `expectThrows(fn, msg)` helpers that print `FAILED:` and `exit(EXIT_FAILURE)`.
- Each test file needs its own `add_executable` + `order_book_enable_warnings` + `order_book_enable_sanitizers` + `add_test` block in `CMakeLists.txt`. New library `.cpp` files must be added to the `order_book` target source list.
- `tests/order_book_test.cpp` uses `#define private public` around includes to seed `OrderBook::bids`/`asks` directly — keep that in mind when renaming private members.

## Architecture

All code lives in namespace `order_book`; headers in `include/order_book/`, implementations in `src/`.

- **Prices are integer ticks** (`unsigned int`), assuming a fixed $0.01 tick size. IDs, sequence numbers, and quantities are also `unsigned int`, and zero is invalid for all of them (`Order` constructor throws `std::invalid_argument`).
- **`Order`** — tracks initial/remaining quantity and `FillStatus` (`NONE`/`PARTIAL`/`FILLED`); `fillQuantity` returns the unfilled leftover of the requested amount.
- **`PriceLevel`** — FIFO `std::list<Order>` at one price/side plus a cached aggregate `quantity`. `addOrder` returns an `OrderLocation`; `consumeQuantity` fills oldest-first and pops fully filled orders. Any mutation must keep `quantity` in sync with the list.
- **`OrderBook`** — `bids` is `std::map<Price, PriceLevel, std::greater>` and `asks` is `std::map<..., std::less>`, so `begin()` is always best price on both sides. `orderIndex` (`unordered_map<OrderId, OrderLocation>`) gives O(1) cancel: `OrderLocation` stores price, side, and a `std::list<Order>::iterator` into the level's list (valid because `std::list` iterators are stable). Empty levels are erased on removal. `snapshot(depth)` returns top-N level summaries per side.
- **Result types** — `SubmissionResults` (trades, remaining qty, `FillStatus`, `SubmissionStatus`, `RejectionReason`), `CancellationResults`, and `Trade` are defined for the future submit/cancel API but not yet wired into `OrderBook`.
- Errors are currently signaled via exceptions (`std::invalid_argument` / `std::runtime_error`); `getBestBid`/`getBestAsk` assume a non-empty side (check `hasBids`/`hasAsks` first).
