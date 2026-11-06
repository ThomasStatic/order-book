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
| `--interval command\|hand-off\|durable-wait\|round-trip\|raw-write\|idle-echo` | the interval to time: one book call (default); into the command ring and back out of the response ring, through a real matching thread; from a record entering a shard's log ring until the durable position covers it, through a real log writer on real files; from the send over loopback through the shipped engine to the frame flagged last; one record's bytes written to a file with no engine; or one message out and back through a raw echo thread watching idle connections too |
| `--writer-core N` | for durable-wait, pin the log writer to this core; needed with `--pin-core` |
| `--durability machine\|process` | for durable-wait, round-trip, and raw-write, the log's level (default `machine`, the engine's) |
| `--log-dir DIR` | for durable-wait, round-trip, and raw-write, where each run makes a fresh directory for its log, deleted afterwards (default: the system's temporary directory) |
| `--log-files extended\|zero-filled` | for durable-wait and raw-write, how files are made: extended (default), as the engine makes them, or zero-filled, every byte written as zeros and flushed before any record |
| `--spin-window TIME\|unlimited` | for hand-off, round-trip, and idle-echo, how long the matching thread, the engine's threads, or the echo thread looks for work before it sleeps, as the engine's `--spin-window` (default `100us`; `0` always sleeps, `unlimited` never does). Hand-off rows record the matching thread's share of a core |
| `--sockets poll\|completion-port\|epoll` | for idle-echo, how the echo thread waits (`poll`, the default, is WSAPoll on Windows; `completion-port` is Windows only and `epoll` Linux only) |
| `--idle-connections N` | for idle-echo and round-trip, connections open beside the measured one that never send (default 0); a round trip's engine takes 63, filling a gateway thread's 64 |
| `--roll-worker on\|off`, `--log-file-records N` | for durable-wait, a roll-forward worker rolling each file the log writer closes while the run goes on, from an empty book's checkpoint, and the records a file holds (default: the engine's 256 MiB), so files close during a run |
| `--server-core N` | for idle-echo, pin the echo thread to this core; needed with `--pin-core` |
| `--matching-core N` | for hand-off and round-trip, pin the matching thread to this core; needed with `--pin-core` |
| `--gateway-core N`, `--receiver-core N` | for round-trip, pin the engine's gateway thread and the timetable's receiving thread; both needed with `--pin-core` |
| `--engine PATH` | for round-trip, the `order_book_engine` to run (default: the one beside the program) |
| `--discard-seconds N` | for round-trip, commands sent in each run's first N seconds are sent and checked but not recorded (default 2) |
| `--time service\|response` | closed-loop service time (default), or response time on a timetable |
| `--rate N` | with `--time response`, one row at N commands a second in place of the ladder, with no capacity measured |
| `--workload NAME` | run this workload; repeat for several (default: all eleven) |
| `--size smoke\|standard\|extended` | 10,000, 100,000, or 1,000,000 (default) commands per run |
| `--commands N` | exactly N commands per run, instead of a size |
| `--runs N` | measured runs per row (default 9, the fewest a row can be quoted from) |
| `--warmup N` | runs before those, discarded (default 1) |
| `--seed N` | workload and timetable seed (default 42) |
| `--noise-seconds N` | seconds of noise floor before and after the session (default 10; 0 skips them, and leaves every row unquotable) |
| `--pin-core N`, `--priority normal\|high` | where the measuring thread runs, and the priority of every pinned thread, the engine's included; a refusal fails the session |
| `--machine-record FILE` | the hand-written machine record, copied into the run record |
| `--out-dir DIR` | write `report.txt`, `runs.csv`, and every run's raw counts under `counts/` |
| `--allow-unoptimized` | run in a build that is not optimized; every row is then unquotable |

Exit statuses: `0` every run completed; `1` a run did not do what its workload predicted, a setting was refused, or a file failed; `2` usage error; `3` the build is not optimized and `--allow-unoptimized` was not given.

## What is timed

**Command**: one `submitOrder`, `cancelOrder`, or `replaceOrder`, from just before the call to just after it returns, on a freshly preloaded book. Loading the command and building its `Order` come before the opening reading, and counting the result and destroying it after the closing one. Every run's outcome is still checked against the workload's prediction, so a run never stands for work the book did not do.

**Hand-off**: from just before a command goes into the command ring until its last event comes back out of the response ring, on the sending thread, through a real matching thread and a real book. The matching thread waits as the engine's does, spinning for `--spin-window` after its last command before it sleeps, and the record gives its share of a core over each run, the median and range: on Windows its cycle count (`QueryThreadCycleTime`), which counts at the TSC's rate, over the TSC's ticks, so a `steady_clock` session leaves it unmeasured, and on Linux its own CPU clock over the run.

**Durable wait**: from just before a record goes into a shard's log ring until the durable position covers it, on the one thread that puts it there, through the engine's own log writer on its own thread, writing real files at `--durability` in a fresh directory under `--log-dir`, deleted after the run. No book runs: the records are the workload's commands, as a matching thread would put them on the ring, so what is timed is the log alone, and the record's book capacity says so. Each record is waited for before the next, as a hand-off waits for each answer, so a closed-loop row is one write per record. A valid run's files, read back through the log's own reader, hold exactly the records sent, in order.

**Round trip**: from a command's send, over loopback through the shipped `order_book_engine`'s gateway and matching thread, to the frame flagged last in its response. The engine runs as a child process on port 0, a fresh one for every run, with one book at the workload's capacity and one connection, so it measures what ships and shares no heap with the client. It logs at `--durability`, in a fresh directory under `--log-dir` that is deleted after the run, so its answers wait for the log as an operator's would, and the record's book capacity is what the engine was given, each part at least 1. The preload goes over the wire before anything is timed. A valid run has every command answered, nothing `Refused`, counts from the frames matching the prediction, and no growth in the stop report. Closed-loop, one thread sends and waits for the last frame. On a timetable, one thread sends and another stamps each read as it arrives: one thread can't both keep a timetable and read. A command's correlation is its index in the timetable, so its response time is the arrival minus its own scheduled send, and no timestamp travels in a frame. A response's first frame gets its own row, since a cascade can put it well before the last. Commands sent in each run's first `--discard-seconds`, by the schedule on a timetable and from the run's start closed-loop, are sent and checked but not recorded, on top of the discarded warm-up run: a placeholder until a round trip's warm-up is measured. The first frame has a row of its own on a timetable only, since a closed-loop row is service time, which no claim about a cascade's first frame rests on.

**Raw write**: one record's 40 bytes, a log record's size, written at the next offset of one file, made as `--log-files` says in a fresh directory under `--log-dir` and opened as the log writer opens its files at `--durability`, with no log writer, ring, doorbell, or chain. Beside the durable wait at the same rate it tells a stall of the drive or the OS, which both show, from one of the engine's, which only the durable wait shows. A valid run's file holds every record written, in order.

**Idle echo**: one 64-byte message out over loopback and back, on the sending thread, which spins on its reply, through a raw echo thread with no gateway, frames, or book. The echo thread watches the active connection and `--idle-connections` more, all for reading, through `--sockets`, and waits as a gateway thread does: looking without sleeping for `--spin-window` after the last message, then sleeping. On completion ports every connection keeps a receive posted. So what the idle connections change is the echo thread's wait alone, which decides whether a socket set other than `poll` is worth adding behind the engine's `SocketSet` seam. A valid run has every message back as sent.

**Service time** runs from when the command actually started. **Response time** runs from when it was scheduled, so it includes any wait behind earlier commands or a stall.

The engine has no timing code, and there are no stage timestamps inside it. Percentiles of separate intervals can't be subtracted, so a report shows intervals side by side and never says "the gateway costs X".

## The clock

One module everywhere: `rdtscp;lfence` where the CPU reports an invariant TSC, calibrated against `steady_clock`, and `steady_clock` otherwise, which includes Arm. Each session records the clock's name, its measured step (the greatest common divisor of successive differences, since Zen 3's TSC advances 39 ticks at a time), the cost of one reading, and the empty pair's p50 and p99.

Every command is timed and nothing is subtracted, since a percentile minus a constant is not the percentile of the difference. Instead each row prints the clock's step and empty pair, and a value within three steps of the empty pair's p50 is marked `unresolved`. Timing every command slows a run, so a per-command timed run never reports throughput.

## Load

Closed-loop only for service time and capacity. A closed-loop client stops sending during a stall, so the commands the stall would have delayed are never measured, and the stall all but vanishes: this is coordinated omission, and `latency_harness_tests` holds Tene's worked example against it.

Everything else runs on a timetable: 10, 25, 50, 75, and 90% of the capacity measured that session, then a seeded Poisson row at 50%, because a fixed timetable hides the queues that bursts build, or one row at `--rate` with no capacity measured, for a row defined by its rate, such as a spin window's gap. Capacity is the median of three closed-loop runs with no per-command stamps: of `core` for the command interval, and of the target itself, one command at a time, for hand-off, durable wait, and round trip, since each waits for its answer. A command is timed from its scheduled send, and a late one goes straight out with nothing skipped. Each timetable row is followed by one of the driver's own lateness, so a stalled driver is not blamed on the book.

A rate prints `not sustained`, in place of any percentile, if the backlog was still growing at the end. A backlog that drains leaves a send on time now and then, however bad the bursts, so the least lateness in a tenth of the run says how far behind the timetable it has fallen for good. If that rose by more than one mean gap across the final two tenths, the run was not sustained, and if any run of a row was not, the row is not. A round trip's sender never waits for an answer, so there the least response time in each tenth stands in for the least lateness.

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

Mandatory, with no default for any field: the interval, service or response time, target and achieved rate; the workload, fingerprint, generator version, and seed; shards, gateway threads, connections, the socket set, spin window, the matching thread's share of a core, core map, pinning, and priority; durability, how the log files were made, and whether logs share a drive; each book's capacity and growths; the clock, its step, and the empty pair; the commands per run, the build, and the hand-written machine record. Where a field does not apply to an interval, the program fills it in saying so.

A row is unquotable, and says why, if a field is unfilled, it has fewer than 9 runs, the build is not optimized, the clock was `steady_clock`, a noise floor is missing, it was not sustained, it was open-loop and not pinned at high priority (printed `unpinned`), or a run recorded nothing. Closed-loop service time may be reported unpinned. A nonzero `capacity-growths` is printed as such, and each growth counts as a tail event.

## Results

Raw per-run CSV and histogram counts stay byte for byte under `benchmarks/results/<date>-latency/`, with a `summarize.py` recomputing every table.
