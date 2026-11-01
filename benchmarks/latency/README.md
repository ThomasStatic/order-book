# Latency harness

`order_book_latency_benchmarks` times one interval, command by command, and reports its percentiles in the only form [`docs/LATENCY.md`](../../docs/LATENCY.md)'s claims rule lets anyone quote. `order_book_benchmarks` measures throughput and never reports latency.

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DORDER_BOOK_BUILD_BENCHMARKS=ON
cmake --build build/release --target order_book_latency_benchmarks
./build/release/order_book_latency_benchmarks --workload matching-heavy --pin-core 5 --priority high --machine-record machine.txt
./build/release/order_book_latency_benchmarks --time response --workload pending-stops --pin-core 5 --priority high --out-dir results
```

With the Visual Studio generator, build and run the Release configuration: `cmake --build build --config Release --target order_book_latency_benchmarks`, then `build/Release/order_book_latency_benchmarks.exe`.

| Option | Effect |
|---|---|
| `--interval command` | the interval to time; only the command interval exists so far |
| `--time service\|response` | closed-loop service time (default), or response time on a timetable |
| `--workload NAME` | run this workload; repeat for several (default: all eleven) |
| `--size smoke\|standard\|extended` | 10,000, 100,000, or 1,000,000 (default) commands per run |
| `--commands N` | exactly N commands per run, instead of a size |
| `--runs N` | measured runs per row (default 9, the fewest a row can be quoted from) |
| `--warmup N` | runs before those, discarded (default 1) |
| `--seed N` | workload and timetable seed (default 42) |
| `--noise-seconds N` | seconds of noise floor before and after the session (default 10; 0 skips them, and leaves every row unquotable) |
| `--pin-core N`, `--priority normal\|high` | where the measuring thread runs; a refusal fails the session |
| `--machine-record FILE` | the hand-written machine record, copied into the run record |
| `--out-dir DIR` | write `report.txt`, `runs.csv`, and every run's raw counts under `counts/` |
| `--allow-unoptimized` | run in a build that is not optimized; every row is then unquotable |

Exit statuses: `0` every run completed; `1` a run did not do what its workload predicted, a setting was refused, or a file failed; `2` usage error; `3` the build is not optimized and `--allow-unoptimized` was not given.

## What is timed

**Command**: one `submitOrder`, `cancelOrder`, or `replaceOrder`, from just before the call to just after it returns, on a freshly preloaded book. Counting the result and destroying it come after the closing reading. Every run's outcome is still checked against the workload's prediction, so a run never stands for work the book did not do.

**Service time** runs from when the command actually started. **Response time** runs from when it was scheduled, so it includes any wait behind earlier commands or a stall.

The engine has no timing code, and there are no stage timestamps inside it. Percentiles of separate intervals can't be subtracted, so a report shows intervals side by side and never says "the gateway costs X".

## The clock

One module everywhere: `rdtscp;lfence` where the CPU reports an invariant TSC, calibrated against `steady_clock`, and `steady_clock` otherwise, which includes Arm. Each session records the clock's name, its measured step (the greatest common divisor of successive differences, since Zen 3's TSC advances 39 ticks at a time), and the empty pair's p50 and p99.

Every command is timed and nothing is subtracted, since a percentile minus a constant is not the percentile of the difference. Instead each row prints the clock's step and empty pair, and a value within three steps of the empty pair's p50 is marked `unresolved`. Timing every command slows a run, so a per-command timed run never reports throughput.

## Load

Closed-loop only for service time and capacity. A closed-loop client stops sending during a stall, so the commands the stall would have delayed are never measured, and the stall all but vanishes: this is coordinated omission, and `latency_harness_tests` holds Tene's worked example against it.

Everything else runs on a timetable: 10, 25, 50, 75, and 90% of the capacity measured that session, then a seeded Poisson row at 50%, because a fixed timetable hides the queues that bursts build. Capacity is the median of three closed-loop runs with no per-command stamps. A command is timed from its scheduled send, and a late one goes straight out with nothing skipped. Each timetable row is followed by one of the driver's own lateness, so a stalled driver is not blamed on the book.

A rate prints `not sustained`, in place of any percentile, if the backlog was still growing at the end. A backlog that drains leaves a send on time now and then, however bad the bursts, so the least lateness in a tenth of the run says how far behind the timetable it has fallen for good. If that rose by more than one mean gap across the final two tenths, the run was not sustained, and if any run of a row was not, the row is not.

## Histograms and what is reported

A hand-written log-linear histogram of raw ticks, 128 sub-buckets per doubling, so a bucket is at most 0.8% of its value wide. Raw counts are saved, so any percentile can be recomputed.

- **Percentiles:** p50, p90, p99, p99.9, and max, from histograms merged over at least 9 runs, each beside its median and range across the runs.
- **The 1,000-beyond rule:** a percentile prints only with 1,000 or more samples beyond it, and each prints that count. p99.9 needs 1,000,000 samples. The merged column and the runs' columns are judged separately.
- **Never:** averaged percentiles, or a dropped run.
- **Max:** the worst, plus the median across runs. No claim rests on it.
- **Warm-up:** one whole run discarded, and every run on a fresh book.

## Noise floor

Every session watches the clock for 10 s before and after, on the same core and at the same priority, recording gaps over 1, 10, and 100 µs per second, the largest, and the share of time lost.

A percentile is marked `noise` when the machine could explain it. A gap over T can put a sample past any value of T or more, so the gaps that could reach a percentile are those over the largest threshold not above it, or over 1 µs for a smaller value. The tail happens `commands per second × (1 - percentile)` times a second, and gaps count as about as frequent within a factor of four, using the worse of the two floors.

## The run record

Mandatory, with no default for any field: the interval, service or response time, target and achieved rate; the workload, fingerprint, generator version, and seed; shards, gateway threads, connections, spin window, core map, pinning, and priority; durability and whether logs share a drive; each book's capacity and growths; the clock, its step, and the empty pair; the commands per run, the build, and the hand-written machine record. Where a field does not apply to an interval, the program fills it in saying so.

A row is unquotable, and says why, if a field is unfilled, it has fewer than 9 runs, the build is not optimized, the clock was `steady_clock`, a noise floor is missing, it was not sustained, or it was open-loop and not pinned at high priority (printed `unpinned`). Closed-loop service time may be reported unpinned. Until the library has `capacityGrowths()`, `capacity-growths` stays unfilled, so no run can be quoted yet.

## Results

Raw per-run CSV and histogram counts stay byte for byte under `benchmarks/results/<date>-latency/`, with a `summarize.py` recomputing every table.
