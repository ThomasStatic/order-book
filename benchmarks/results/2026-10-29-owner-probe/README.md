# Owner-probe check (spec #40, P2)

Raw output of `benchmarks/compare_ab.py`, merged per directory but otherwise as it wrote it: each directory holds `summary.md` and `runs.csv`, every invocation's CSV rows concatenated with two leading columns: `pair` (its number) and `binary` (`base` or `exp`). Written up in [`docs/OPTIMIZATION.md`](../../../docs/OPTIMIZATION.md#the-owner-probe-v200).

Release build, `/O2 /Ob2 /DNDEBUG`, Visual Studio 17 2022 generator, MSVC 19.44, on the reference machine of `benchmarks/README.md`; standard size, seed 42, `core` only, generator version 3; each invocation 1 warm-up and 5 measured runs, in ABBA pairs.

| Directory | Base | Experiment | Pairs |
|---|---|---|---:|
| `owned-vs-request` | `matching-heavy-request`, owner in the index slot (commit 73f7c92) | `matching-heavy-owned`, same binary | 16 |
| `owned-vs-request-32` | the same | the same | 32 |
| `null-request` | `matching-heavy-request` | the same workload, same binary | 16 |
| `request-vs-v1` | `matching-heavy` (v1 path) | `matching-heavy-request`, same binary | 16 |
| `node-owned-vs-request-32` | `matching-heavy-request`, owner in the queue node | `matching-heavy-owned`, same binary | 32 |
| `node-vs-slot` | 73f7c92 (owner in the index slot) | the owner in the queue node | 16 per workload |

Reproduce, for example:

```bash
python benchmarks/compare_ab.py --base build/Release/order_book_benchmarks.exe --exp build/Release/order_book_benchmarks.exe \
    --out owned-vs-request-32 --kind core --pairs 32 --workload matching-heavy-request --exp-workload matching-heavy-owned
```
