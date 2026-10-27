# Profile of the baseline, 2026-10-26

Raw output of the profiling tools in `benchmarks/profiling/` (see its README) on the code of the benchmark baseline (`docs/BENCHMARKS.md`), written up in `docs/PROFILING.md`. The profiled binary is the baseline's Release build plus symbols (`/Zi`, `/DEBUG /OPT:REF /OPT:ICF`), on the same machine, with the build directory `build/profile` configured as the tools' README shows.

- `allocations.txt`: `allocation_profile --size standard`, every workload, both paths.
- `<workload>-core.txt`, `<workload>-core.folded`: sampling profile of the core path, extended size, 20 runs, 1 ms interval. All five workloads.
- `<workload>-replay.txt`, `<workload>-replay.folded`: the same for the replay path, 8 runs. `small-book`, `deep-book`, and `matching-heavy`.
- `<workload>-<kind>.benchmark.txt`: the benchmark program's own output while profiled; its throughput is 5-27% below the unprofiled baseline (`small-book` least, `cancellation-heavy` most).
- `profiler-log.txt`: the profiler's console output (sample counts and exit codes).

Commands, run from the repository root:

```bash
OUT=benchmarks/results/2026-10-26-profile
P=build/profile/Release/sampling_profiler.exe
B=./build/profile/Release/order_book_benchmarks.exe
DROPS="--drop preload --drop generateWorkload --drop recordFinalBook --drop renderReplayCsv --drop describeOutcome --drop ~OrderBook"
build/profile/Release/allocation_profile.exe --size standard > $OUT/allocations.txt
for w in small-book deep-book matching-heavy cancellation-heavy mostly-resting; do
  $P --interval-us 1000 --out $OUT/$w-core --root runCore --root runReplay $DROPS \
    -- $B --workload $w --size extended --kind core --warmup 0 --repetitions 20 --seed 42 \
    > $OUT/$w-core.benchmark.txt 2>> $OUT/profiler-log.txt
done
for w in small-book deep-book matching-heavy; do
  $P --interval-us 1000 --out $OUT/$w-replay --root runCore --root runReplay $DROPS \
    -- $B --workload $w --size extended --kind replay --warmup 0 --repetitions 8 --seed 42 \
    > $OUT/$w-replay.benchmark.txt 2>> $OUT/profiler-log.txt
done
```
