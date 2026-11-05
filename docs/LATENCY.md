# Latency

What one command takes, measured under the rules below. **No baseline has been recorded yet**: this report holds only the three rows that decide a default before one is (V1, #96). The method is in [`benchmarks/latency/README.md`](../benchmarks/latency/README.md).

## The claims rule

Every statement drawn from these results follows this rule, and so does anything quoting them.

**A report may say:** on this named machine and build, with this run record, this interval had these percentiles at this rate, and a change moved them by more than A/A.

**A report must refuse:**

- "low latency", "microsecond", or "exchange grade", in any form;
- any comparison with an exchange or another system;
- "wire-to-wire" or anything implying a network, since loopback is not one;
- a number without its interval and run record, or a single headline number;
- subtracting one interval from another;
- attributing a marked noise-floor percentile to the engine, or treating max as a guarantee;
- any `not sustained` rate quoted as a capability;
- any Linux or other-machine claim drawn from these runs, and any number from the Linux sanity workflow, which runs on shared GitHub runners (no Linux numbers are recorded for v2.1.0);
- quoting a `steady_clock` result or the throughput harness's ns/command as latency.

## Intervals

| Interval | From | To |
|---|---|---|
| **Command** | Before one book call | After it returns |
| **Hand-off** | Into the command ring | Back out of the response ring |
| **Round trip** | Scheduled send, over loopback through the shipped engine | Frame flagged last |
| **Durable wait** | A record enters the log ring | The durable position covers it |

All four intervals exist. A pinned hand-off row pins both threads, the sender with `--pin-core` and the matching thread with `--matching-core`. A pinned durable-wait row pins both too, the sender and the log writer with `--writer-core`. A pinned round-trip row pins four: the sender, the receiver with `--receiver-core`, and the engine's matching and gateway threads with `--matching-core` and `--gateway-core`, leaving the engine's log writer and housekeeping thread unpinned. A durable-wait or round-trip row names its durability, `machine` unless `--durability process` says otherwise, and says no checkpoints share the log's drive, since none are written yet.

## Results

No baseline is recorded yet. What follows are V1's three decision rows (#96, rules from #33). They were measured on 2026-11-05 on the reference machine: an AMD Ryzen 5 5600G with 6 cores, SMT on, and 31.8 GiB; a WD Blue SN570 NVMe drive, the machine's only drive; Windows 11 Pro 10.0.26200 on the High performance plan. The build was MSVC Release. Every row is `small-book`, and every pinned thread runs at high priority.

The raw results are in [`benchmarks/results/2026-11-05-latency-v1/`](../benchmarks/results/2026-11-05-latency-v1/): each session's `report.txt` with its run record, `runs.csv`, and the raw counts. `run.sh` and `run_unpinned.sh` made them, and `summarize.py` recomputes every figure below. Each row decides a default; none is a latency claim beyond its own run record.

### Spin window

The interval is hand-off response time on a fixed timetable: the gap is the time between scheduled sends. The sender is pinned to logical processor 6 and the matching thread to 10, on different physical cores. Each cell is the merged p50 over 9 runs, then the matching thread's share of a core in the median run.

| Window | 20 µs gap | 100 µs gap | 1 ms gap | 20 ms gap |
|---|---|---|---|---|
| `0` | 3.38 µs, 22.7% | 3.94 µs, 4.9% | 5.18 µs, 0.6% | 17.20 µs, 0.1% |
| `20us` | 0.43 µs, 99.8% | 3.61 µs, 24.6% | 4.76 µs, 2.6% | 17.07 µs, 0.2% |
| `100us` | 0.40 µs, 99.8% | 0.51 µs, 99.8% | 4.53 µs, 10.5% | 17.72 µs, 0.6% |
| `1ms` | 0.42 µs, 99.8% | 0.42 µs, 99.9% | 0.68 µs, 99.9% | 17.46 µs, 5.1% |
| `unlimited` | 0.42 µs, 99.9% | 0.43 µs, 99.9% | 0.85 µs, 99.8% | 4.86 µs, 99.8% |

At the 20 ms gap, each run holds 250 commands. The merged p50 therefore prints with only 1,125 samples beyond it, and it is marked `noise`.

**The rule:** the default becomes the smallest window whose p50 at the 20 µs gap is within A/A of `unlimited`.
- **The A/A band.** Three `unlimited` sessions were run at the 20 µs gap: in the sweep, straight after it, and after every other row. They gave 0.42, 0.41, and 0.41 µs.
- **What "within" means here.** The rule asks for a window that loses nothing against never sleeping. So a window counts as within A/A when its p50 is no slower than the band's top, 0.42 µs. Read two-sided, the rule would refuse a window for beating `unlimited`.
- **Each window:**
  - `0` (3.38 µs) is outside.
  - `20us` (0.43 µs) is outside: at a 20 µs gap its matching thread sometimes sleeps just before the next command arrives.
  - `100us` (0.40 µs) is within.
- **The result:** the default stays at `100us`. A two-sided reading would have chosen `1ms`, which keeps the matching thread at 99.9% of a core at a 1 ms gap, against `100us`'s 10.5%.

### Idle connections

The interval is a raw loopback echo with no gateway: one 64-byte message out and back, closed-loop service time, 9 runs of 50,000. The echo thread is on logical processor 10. Each cell is the merged p50.

| Socket set, echo thread's spin window | 0 idle | 16 idle | 64 idle | 256 idle | 1,024 idle |
|---|---|---|---|---|---|
| `WSAPoll`, `100us` | 6.14 µs | 8.86 µs | 17.72 µs | 71.94 µs | 562.94 µs |
| `WSAPoll`, `0` | 6.01 µs | 8.80 µs | 17.72 µs | 71.94 µs | 562.94 µs |
| completion ports, `100us` | 5.81 µs | 5.74 µs | 5.78 µs | 5.71 µs | 5.78 µs |
| completion ports, `0` | 5.78 µs | 5.68 µs | 5.81 µs | 5.58 µs | 5.71 µs |

**The rule:** at 64 idle connections, if `poll`'s p50 is more than twice the alternative's, the docs recommend that adapter and a follow-up issue is filed.
- **The result on Windows:** at the shipped spin window, `WSAPoll` is 17.72 µs against 5.78 µs on completion ports, 3.07 times. The rule fires.
- **The cost of each idle connection.** `WSAPoll` costs grow with every connection the gateway thread watches, about 0.18 µs per idle connection at 64 and 0.55 µs at 1,024. Completion ports stay flat.
- **What this means for operators.** The engine still ships `poll` only, capped at 64 connections per gateway thread. On Windows, a deployment with many connections should spread them over more gateway threads until the completion-port adapter lands: [#105](https://github.com/ThomasStatic/order-book/issues/105).
- **Linux.** The Linux verdict, `poll` against epoll, comes from the Linux sanity workflow, for large effects only, and is never quoted. Its verdict is recorded on #105 once the workflow has run.

### 10k/s durable wait

The interval is durable wait response time at `machine`, at 10,000 records a second: five 60-second runs, 3,000,000 samples. Beside it runs the raw write-through loop at the same rate: one record per write, with no log writer, ring, or chain. Both get the same preparation, the current file and one made ahead, both extended or both zero-filled. With 5 runs and not 9, every row here is unquotable, and serves only the rule.

**The rule:** if the raw loop stalls the same way, it is the drive or the OS, documented as the environment. If only the engine stalls, it is a bug to find before the baseline is recorded. A stall here is the tail standing far off the middle, p99 or p99.9 over p50, more than twice the raw loop's ratio.

Unpinned, as the engine runs its log writer, with the merged figures over five runs:

| Row | Files | p50 | p99 | p99.9 | max | Each run's max: median, range |
|---|---|---|---|---|---|---|
| durable wait | extended | 83.5 µs | 147.3 ms | 190.4 ms | 197.3 ms | 196.2 ms, 132.3 to 197.3 ms |
| raw write loop | extended | 83.0 µs | 143.0 ms | 197.9 ms | 241.8 ms | 192.7 ms, 8.2 to 241.8 ms |
| durable wait | zero-filled | 43.6 µs | 82.4 µs | 213.2 µs | 13.4 ms | 1.9 ms, 0.7 to 13.4 ms |
| raw write loop | zero-filled | 43.1 µs | 87.2 µs | 146.3 ms | 186.4 ms | 186.3 ms, 0.6 to 186.4 ms |

**The result:** the raw loop stalls the same way on both kinds of file, so the stalls are the drive or the OS, and no engine-only stall stands in the way of the baseline.
- **Extended files:** p99 over p50 is 1,765 against the raw loop's 1,724.
- **Zero-filled files:** p99 over p50 is 1.9 against 2.0, and p99.9 over p50 is 4.9 against 3,397, where the raw loop met a drive stall the durable wait did not.

**The drive.** On extended files, as the engine makes them, write-through stalls on this drive reach 0.2–0.3 s, in the engine and in the raw loop alike, at p99 and beyond. Zero-filled files stall far less. ADR 0013's throwaway medians had extended files about 30 µs dearer per write, and the p50s here agree: 83.5 µs against 43.9 µs in the pinned durable wait. Those were medians, though, and this is the tail. That makes this drive and OS the environment for every `machine` row here: a durable-wait p99 on extended files is the drive's, and no figure from it describes the engine.

**Pinning the log writer.** The first pass pinned the log writer to a logical processor with the harness's `--writer-core`, as the hand-off row pins its matching thread. On zero-filled files the writer alone then stalled:
- In six interleaved single runs, every pinned engine run had a worst wait of 18.6 to 325 ms. The raw loop's worst in each of its runs was 1.3 to 2.3 ms.
- The same happened with the writer on logical processors 4 and 10. On 8, one run was clean.
- A writer that never slept stalled too, so the cause is not its wake-up.
- Unpinned, the writer's runs were as clean as the raw loop's.

The shipped engine never pins its log writer: it has no option to. So this is a cost of pinning a spinning high-priority thread on this machine, not the engine's. The pinned sessions and the interleaved runs are kept in the results, under `diagnosis/`. A pinned durable-wait row should not be read as the engine's.

## What can exceed these numbers

Three things can take a command past every percentile here. Their figures come from `order_book_component_benchmarks`, and none is recorded yet.

- A capacity growth.
- A new level far from the touch.
- An image flush on the log's drive.
