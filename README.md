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
./build/debug/order_book_cli
```

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
