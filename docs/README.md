# Design Notes

Design notes and reports. The matching rules and invariants themselves are documented where they are enforced, in `include/order_book/order_book.hpp` and `include/order_book/invariants.hpp`.

- [`replay-format.md`](replay-format.md): the CSV command format for replaying a session through the book: columns, field syntax, sequence ordering, which layer rejects what, and the fail-fast error policy; then the replay engine, its statistics and summary, and `order_book_cli replay`. Sample files and expected output are in `tests/replay/`.
- [`BENCHMARKS.md`](BENCHMARKS.md): the recorded benchmark baseline: results for every workload at the standard and extended sizes, which percentiles are and are not reported and why, methodology, and limitations. Raw results are in `benchmarks/results/`.
- [`PROFILING.md`](PROFILING.md): where the baseline spends its time, from a sampling profile and exact allocation counts, and the three strongest bottleneck hypotheses.
- [`OPTIMIZATION-EXPERIMENTS.md`](OPTIMIZATION-EXPERIMENTS.md): one designed experiment per candidate optimization, the paired measurement protocol, and each experiment's measured outcome, including the reverted ones.
- [`images/`](images/): the figures in the README and these reports. `make_figures.py` redraws the three data-driven ones (benchmark baseline, optimization, replay terminal) from the result files and the CLI's expected output; the four diagrams are edited as SVG.
- [`OPTIMIZATION.md`](OPTIMIZATION.md): the optimization report: what profiling revealed, before-and-after measurements and profiles, one kept and one rejected change in detail, the readability trade-offs, what remains unoptimized, and how correctness was checked.
