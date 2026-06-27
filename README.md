# Order Book

A correctness-first C++ limit-order-book and matching-engine project.

## Status

Project scaffold complete. The first implementation milestone is limit orders, price-time priority, partial fills, cancellations, and trade events.

## Layout

- `include/` — public headers
- `src/` — implementation
- `tests/` — unit tests
- `benchmarks/` — throughput and latency experiments
- `docs/` — design notes

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
