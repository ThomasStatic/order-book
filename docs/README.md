# Design Notes

This directory will contain architecture decisions, matching rules, and invariants.

- [`replay-format.md`](replay-format.md): the CSV command format for replaying a session through the book: columns, field syntax, sequence ordering, which layer rejects what, and the fail-fast error policy; then the replay engine, its statistics and summary, and `order_book_cli replay`. Sample files and expected output are in `tests/replay/`.
- [`BENCHMARKS.md`](BENCHMARKS.md): the recorded benchmark baseline: results for every workload at the standard and extended sizes, which percentiles are and are not reported and why, methodology, and limitations. Raw results are in `benchmarks/results/`.
