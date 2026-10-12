# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A correctness-first C++20 limit-order-book and matching-engine library (`order_book` static lib), with a placeholder CLI (`order_book_cli`) and optional benchmarks. The storage layer (price levels, order index, snapshots) and price-time-priority matching via `submitOrder` are implemented; `SubmissionResults`/`CancellationResults` are not yet wired in.

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

Run a single test by CTest name with `-R` (names: `order_book_smoke`, `order_tests`, `result_type_tests`, `price_level_tests`, `execution_tests`, `allocation_failure_tests`), e.g. `ctest --test-dir build -C Debug -R price_level_tests --output-on-failure`, or build and run one target directly (`cmake --build build --config Debug --target price_level_tests`).

CMake options: `ORDER_BOOK_BUILD_BENCHMARKS`, `ORDER_BOOK_ENABLE_ASAN` (GCC/Clang/MSVC; on MSVC it strips `/RTC1` from Debug flags and copies `clang_rt.asan_dynamic-*.dll` next to each executable), `ORDER_BOOK_ENABLE_UBSAN` (GCC/Clang only; ignored with a warning on MSVC), `ORDER_BOOK_WARNINGS_AS_ERRORS`. Warnings are high (`/W4`, or `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`), so watch for sign/narrowing conversions.

## Tests

- No test framework: each `tests/*.cpp` is its own executable with a `main()` using local `expect(cond, msg)` / `expectThrows(fn, msg)` helpers that print `FAILED:` and `exit(EXIT_FAILURE)`.
- Each test file needs its own `add_executable` + `order_book_enable_warnings` + `order_book_enable_sanitizers` + `add_test` block in `CMakeLists.txt`. New library `.cpp` files must be added to the `order_book` target source list.
- `tests/allocation_failure_test.cpp` replaces global `operator new` to fail each allocation in turn and checks the book stays consistent (e.g. `addOrder` rolls back). MSVC Debug builds' checked iterators allocate inside `noexcept` container members, so there it exits 77 and CTest reports it **Skipped**; run `cmake --build build --config Release` and `ctest --test-dir build -C Release` to exercise it.
- `tests/order_book_test.cpp` uses `#define private public` around includes to seed `OrderBook::bids`/`asks` directly — keep that in mind when renaming private members. The hack only works for private **data members** on MSVC: access level is part of MSVC's mangled function names, so calling a private member function this way fails to link (LNK2019). To test private member functions, use the `OrderBookTestAccess` friend (declared in `OrderBook` and `PriceLevel`, defined in `tests/execution_test.cpp`) and don't combine it with the `#define` hack in the same file.

## Architecture

All code lives in namespace `order_book`; headers in `include/order_book/`, implementations in `src/`.

- **Prices are integer ticks** (`unsigned int`), assuming a fixed $0.01 tick size. IDs, sequence numbers, and quantities are also `unsigned int`, and zero is invalid for all of them (`Order` constructor throws `std::invalid_argument`).
- **`Order`** — tracks initial/remaining quantity and `FillStatus` (`NONE`/`PARTIAL`/`FILLED`); `fillQuantity` returns the unfilled leftover of the requested amount.
- **`PriceLevel`** — FIFO `std::list<Order>` at one price/side plus a cached aggregate `quantity`. `addOrder` returns an `OrderLocation`; `consumeQuantity` fills oldest-first, pops fully filled orders, and returns a `ConsumptionResult` (status, per-order `OrderFill`s, unfilled remainder). Note the status naming: `SATISFIED` means the level is now empty, `LEVEL_EXHAUSTED` means quantity still rests there. Any mutation must keep `quantity` in sync with the list. The private `fillOldestOrder` (fills one order, popping it if fully filled) is reserved for its friend `OrderBook`. Don't call `consumeQuantity` on a level inside a book directly — go through `OrderBook::consumeLevel` (multi-order fill at one level, currently used only by tests), which loops `fillOldestOrder` itself, unindexing each fully filled order the moment it leaves the level, and erases the level once empty.
- **`OrderBook`** — `bids` is `std::map<Price, PriceLevel, std::greater>` and `asks` is `std::map<..., std::less>`, so `begin()` is always best price on both sides. `orderIndex` (`unordered_map<OrderId, OrderLocation>`) gives O(1) cancel: `OrderLocation` stores price, side, and a `std::list<Order>::iterator` into the level's list (valid because `std::list` iterators are stable). `addOrder` and `submitOrder` both run `validateNewOrder` first, rejecting (`std::invalid_argument`, before any mutation) an order with no remaining quantity or marked `FILLED`, or whose ID is already resting; IDs of filled or cancelled orders may be reused, since the index tracks only live orders. `findActiveOrder`, `getBestBid`, and `getBestAsk` return `Order` copies (never a reference or iterator), so callers can neither mutate a resting order behind the level's cached `quantity` nor hold a reference that dangles once it is filled or removed; tests that need element identity use `OrderBookTestAccess::indexedOrder`. `OrderBook` is move-only: a copy would share the original's list iterators. Empty levels are erased on removal. `snapshot(depth)` returns top-N level summaries per side.
- **Matching** — `submitOrder` runs `matchOrder` (loops while the incoming order has quantity and crosses the best opposing price), then rests any remainder via `addOrder`. Each loop step is one private `executeOnce`: it executes min(incoming, resting) against the oldest order at the best opposing level, at the resting price, and returns exactly one `Trade`. It validates everything and builds the `Trade` before mutating, so a throw leaves the book unchanged — preserve that ordering. `matchOrder` makes room in its trade vector before each `executeOnce`, so recording a trade cannot throw after the book changed. `submitOrder` is not atomic: if an allocation fails mid-match or while resting the remainder, executions already made stand (the book stays consistent) and their trades are lost with the exception. It unindexes the resting order only when fully filled (a partially filled one keeps its index entry and FIFO position) and erases the level once empty.
- **Result types** — `SubmissionResults` (trades, remaining qty, `FillStatus`, `SubmissionStatus`, `RejectionReason`) and `CancellationResults` are defined for the future submit/cancel API but not yet wired into `OrderBook`. `Trade` is returned by `submitOrder`; it stores resting (maker) and incoming (taker) IDs plus the incoming side, from which `getBuyOrderId`/`getSellOrderId` derive.
- Errors are currently signaled via exceptions (`std::invalid_argument` / `std::runtime_error`); `getBestBid`/`getBestAsk` throw `std::runtime_error` on an empty side.
