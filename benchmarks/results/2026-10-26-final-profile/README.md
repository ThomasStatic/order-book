# Profile of the optimized code, 2026-10-26

Raw output of the profiling tools in `benchmarks/profiling/` (see its README) on the code after the optimization experiments (`docs/OPTIMIZATION-EXPERIMENTS.md`), written up in `docs/OPTIMIZATION.md`. Same machine, build flags (`build/profile`), workloads, sizes, and run counts as the baseline's profile in `../2026-10-26-profile/`, so the two can be compared row by row.

- `allocations.txt`: `allocation_profile --size standard`, every workload, both paths. The order index is an open-addressed table here, so its lines report growths, slots, and fill rather than rehashes, buckets, and load factor.
- `<workload>-core.txt`, `<workload>-core.folded`: sampling profile of the core path, extended size, 20 runs, 1 ms interval. All five workloads.
- `<workload>-replay.txt`, `<workload>-replay.folded`: the same for the replay path, 8 runs. `small-book`, `deep-book`, and `matching-heavy`.
- `<workload>-<kind>.benchmark.txt`: the benchmark program's own output while profiled.
- `profiler-log.txt`: the profiler's console output (sample counts and exit codes).

Commands, run from the repository root in Git Bash. Two drops more than the baseline's: since the node pool, `~OrderBook` is inlined into `runCore`, so the book's destruction after the clock stops appears as the level maps' and the pool's destructors (see the profiler's README).

```bash
OUT=benchmarks/results/2026-10-26-final-profile
P=build/profile/Release/sampling_profiler.exe
B=./build/profile/Release/order_book_benchmarks.exe
DROPS=(--drop preload --drop generateWorkload --drop recordFinalBook --drop renderReplayCsv --drop describeOutcome
       --drop '~OrderBook' --drop 'order_book::PriceLevel> >,0> >::{dtor}' --drop '~unique_ptr<order_book::NodePool')
build/profile/Release/allocation_profile.exe --size standard > $OUT/allocations.txt
for w in small-book deep-book matching-heavy cancellation-heavy mostly-resting; do
  $P --interval-us 1000 --out $OUT/$w-core --root runCore --root runReplay "${DROPS[@]}" \
    -- $B --workload $w --size extended --kind core --warmup 0 --repetitions 20 --seed 42 \
    > $OUT/$w-core.benchmark.txt 2>> $OUT/profiler-log.txt
done
for w in small-book deep-book matching-heavy; do
  $P --interval-us 1000 --out $OUT/$w-replay --root runCore --root runReplay "${DROPS[@]}" \
    -- $B --workload $w --size extended --kind replay --warmup 0 --repetitions 8 --seed 42 \
    > $OUT/$w-replay.benchmark.txt 2>> $OUT/profiler-log.txt
done
```
