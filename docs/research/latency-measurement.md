# How matching-engine latency is measured and reported

Research for [issue #23](https://github.com/ThomasStatic/order-book/issues/23), which blocks [#32](https://github.com/ThomasStatic/order-book/issues/32), on the wayfinder map [#37](https://github.com/ThomasStatic/order-book/issues/37). Read 2026-10-28.

This note records what primary sources say about timing a matching engine or messaging path: what is timed, which clocks are used, coordinated omission, which percentiles are reported and from how many samples, warm-up and variance, and what claims the sources make or refuse to make. It also records what this repo's benchmark methodology already fixes. It recommends no design.

Citations are reference links (for example [tene-2012]), resolved in [Sources](#sources). "Not stated" means the pages read say nothing about it. Nothing has been filled in from memory. Arithmetic that is not from a source is labelled as such.

## Facts the decisions wait on

- **Clock cost against command cost.** Microsoft gives QPC on a TSC-backed system an access time of about 30 ns and says measurement precision is the larger of resolution and access time [ms-qpc]. MSVC's `steady_clock::now()` wraps QPC and has a fast path for 10 MHz, "a very common QPC frequency" [msvc-chrono]. A 10 MHz QPC has a 100 ns tick. The repo's commands cost about 150 to 1,100 ns each on the reference machine [repo-bench], so a per-command timing with `steady_clock` on that machine would be within one or a few ticks.
- **Coordinated omission decides whether a tail number means anything.** A closed-loop driver (send, wait, send) stops sampling while the system is stalled. Tene's worked example turns a true 99.99th percentile of about 100 s into a reported 1 ms [tene-2012]. Open-loop drivers (wrk2, the Aeron rig, Chronicle's test) timestamp each request with the time it *should* have been sent [wrk2] [aeron-rig] [chronicle-2017]. HdrHistogram can also correct after the fact when the expected interval is known [hdr-readme].
- **Sample counts set the highest percentile that can be reported.** The sources that publish tails run large sample counts: Disruptor 50 million events [disruptor-paper], Chronicle 20 million messages [chronicle-2017], Aeron ten one-second iterations at a fixed rate [aeron-config]. Arithmetic, not from a source: a percentile p has (1 - p) x N samples beyond it, so p99.99 needs 10,000 samples per sample in the tail.
- **Where the clock starts and stops differs by orders of magnitude between sources.** Disruptor times an in-process queue hand-off [disruptor-paper]; Aeron and Chronicle time a round trip between threads or processes [aeron-readme] [chronicle-2017]; SIX (on Nasdaq's X-stream INET) times a round trip from the exchange network boundary to the matching engine and back, captured on the wire [six-inet]. None of these is the cost of one in-process `submitOrder` call.
- **The repo already refuses latency claims.** `docs/BENCHMARKS.md` reports per-run throughput only, states that per-command percentiles were not measured, and rules out "low latency", "microsecond", "exchange grade", tail-latency claims, and comparison with exchanges [repo-bench].

## 1. What is timed

| Source | Start of interval | End of interval | Where the clock is read |
|---|---|---|---|
| LMAX Disruptor paper | Event injected into a three-stage pipeline | Event processed at the end of the pipeline | CPU time stamp counter, in process; reported per hop and for the pipeline [disruptor-paper] |
| Aeron benchmarks | Scheduled send time, written into the message | Echoed message received | Round trip between threads or processes (IPC), or to a remote echo node [aeron-readme] [aeron-rig] |
| Chronicle Queue (2017) | Time the message "should have been sent", carried in the message | Reader thread reads it | Writer thread to reader thread through a queue file [chronicle-2017] |
| JLBH (Chronicle's harness) | Start of each benchmark iteration; optional probes time stages | End of iteration | "end to end" summary plus per-probe histograms [jlbh] |
| wrk2 | Time the request *should* have been sent under the configured rate | Response received | HTTP client, over the network or loopback [wrk2] |
| SIX X-stream INET (Nasdaq platform) | Order crosses the SIX network domain boundary (MP1) | Acknowledgement or fill crosses back (MP2), or ITCH output (MP3) | Corvil on the 10 Gbit co-location link outside the boundary; "This includes the time it takes to validate, process, and acknowledge or fill a participant order" [six-inet] |
| Turquoise (LSE) 2010, trade press | Order entry | Accepted, processed, and acknowledged | Not stated in the article read [tradenews-2010] |
| This repo | Before the first timed command of a run | After the last | `std::chrono::steady_clock`, twice per run [repo-readme] |

- Disruptor's latency test paces its injector: "waiting 1 microsecond after injecting an event before injecting the next and repeating 50 million times", "at less than saturation" [disruptor-wiki].
- Aeron's rig sends at "the specified fixed rate with the specified payload size and the burst size" [aeron-readme]. Every message must be at least 16 bytes "since every message must contain a timestamp payload" [aeron-config].
- HdrHistogram cites recording costs of "3-6 nanoseconds" on Intel CPUs of about 2012 [hdr-readme] (circa 2014 on the project page [hdr-site]).

## 2. Clocks on Windows and Linux

### Windows: QueryPerformanceCounter

From Microsoft's "Acquiring high-resolution time stamps" [ms-qpc]:

- QPC "is typically the best method to use to time-stamp events and measure small time intervals that occur on the same system or virtual machine". It is a difference clock, independent of system time and UTC.
- Monotonic: "Yes. QPC does not go backward." Values from different threads that differ by plus or minus 1 tick "have an ambiguous ordering". Within one thread that uncertainty does not apply.
- Basis: since Windows 7, mostly the TSC where the TSC is constant-rate and synchronized across processors. Otherwise Windows falls back to HPET or the ACPI PM timer. With an invariant TSC, QPC is not affected by power management or Turbo Boost.
- Cost: with the TSC as basis, "10s of CPU cycles to several hundred CPU cycles", and no kernel transition. With a platform timer, a system call and "frequently in the vicinity of 0.8 - 1.0 microseconds" per call.
- Frequency: fixed at boot. Under a hypervisor with the 1.0 interface, "or always in some newer versions of Windows", it is fixed at 10 MHz, so "don't assume that QueryPerformanceFrequency will return a value derived from the hardware frequency."
- Precision is "MAX [ Resolution, AccessTime]". The doc's table gives QPC on a 3 GHz TSC a 333 ns resolution and 30 ns access time (precision 333 ns), and raw RDTSC a 333 ps resolution and 30 ns access time (precision 30 ns). Quantization error is plus or minus 1 tick, which matters "if the period being measured is small and approaches the resolution of the timer".
- RDTSC directly: "We strongly discourage using the RDTSC or RDTSCP processor instruction to directly query the TSC", because results are unreliable on some Windows versions, across live VM migration, and on hardware without invariant or synchronized TSCs. Out-of-order execution can also misplace an RDTSC read; RDTSCP was introduced in response.
- Thread affinity is not needed for QPC; pinning to use it is "neither necessary nor desirable".

### The TSC itself

- Intel SDM, Invariant TSC: "Processor's support for invariant TSC is indicated by CPUID.80000007H:EDX[8]." "The invariant TSC will run at a constant rate in all ACPI P-, C-. and T-states." Read from an unofficial HTML mirror of Volume 3 (section 17.15.1 in that edition); not checked against the current SDM [intel-sdm-mirror].
- Linux kernel KVM timekeeping doc: the TSC may stop in deep C-states; "It is recommended not to trust the TSCs to remain synchronized on NUMA or multiple socket systems"; "Forcing serialized execution is necessary for precise measurement with the TSC" (CPUID or similar). Linux feature bits: `CONSTANT_TSC` ("The TSC rate is unchanged with P-states") and `NONSTOP_TSC` ("The TSC does not stop in C-states") [kernel-tsc].
- The Disruptor paper chose "CPUs with an invariant TSC because older processors suffer from changing frequency", and used no CPU binding [disruptor-paper].

### Linux: clock_gettime

From the `clock_gettime(2)` man page [man-clock]:

- `CLOCK_MONOTONIC` "is not affected by discontinuous jumps in the system time" but "is affected by frequency adjustments" (NTP), and does not count suspend. `CLOCK_MONOTONIC_RAW` is "not subject to frequency adjustments". All monotonic variants never go backwards, but successive calls may return identical values.
- "On some architectures, an implementation of clock_gettime() is provided in the vdso(7)." Resolution comes from `clock_getres`; the man page's example prints 1 ns for each clock.
- The vDSO exists because frequent calls such as `gettimeofday` would otherwise pay "the context-switch overhead that results from exiting user space and entering the kernel"; with it a call becomes "a normal function call and a few memory accesses". x86-64 exports `__vdso_clock_gettime` [man-vdso].
- Neither page names the hardware clocksource or gives a per-call cost.

### std::chrono::steady_clock

- **MSVC STL:** `struct steady_clock { // wraps QueryPerformanceCounter`, `period = nano`, `is_steady = true`. `now()` reads `_Query_perf_frequency()` and `_Query_perf_counter()`. A fast path for 10 MHz "can double the performance of this function by avoiding the expensive frequency conversion path"; 24 MHz is special-cased for ARM64 [msvc-chrono]. The nanosecond `period` is the unit, not the resolution: the tick is the QPC period (arithmetic: 100 ns at 10 MHz).
- **libstdc++:** `steady_clock::now()` calls `clock_gettime(CLOCK_MONOTONIC, &tp)` (or the raw syscall when `_GLIBCXX_USE_CLOCK_GETTIME_SYSCALL` is defined), and falls back to `system_clock` only when `CLOCK_MONOTONIC` is unavailable [libstdcxx-chrono].
- **This repo's reference machine:** `steady_clock` is "MSVC's QueryPerformanceCounter clock at 10 MHz, a 100 ns tick" [repo-readme].

## 3. Coordinated omission

- **Definition (Tene, 2013):** omission is harmless when random, but "when omission is coordinated with observed events, it can dramatically skew the statistical analysis". His example: a thread calling a logger about every 1 ms that stalls for 536 ms misses about 535 observations; from ten visible outliers he estimates a 99.99th percentile "at least 35,000x higher than reported" [tene-co]. Causes named in the thread: load testers that wait for each response before sending the next request; the effect is per thread and worse with fewer threads. Martin Thompson's reply describes an injector stalling in Disruptor latency tests [tene-co].
- **The canonical example (Tene, QCon SF 2012 slides) [tene-2012]:** a system handles 100 requests/s at 1 ms each for 100 s, then stalls for 100 s. A closed-loop tester records 10,000 results at 1 ms and one at 100 s, giving "99.99%`ile is 1 msec! Average. is 10.9msec! Std. Dev. is 0.99sec!". Measured properly, the stall holds 10,000 results varying linearly from 100 s down to 10 ms, so the 50th percentile is about 1 ms, the 75th about 50 s, the 99.99th about 100 s, and the average about 25 s. The slide on the simple load tester says it "works well only when all responses fit within rate interval" and that the "technique includes implicit 'automatic backoff' and coordination".
- **Open-loop fix (wrk2):** measures "from the time the transmission *should* have occurred" under a constant rate (`-R`). Its README shows a 1.4 s disruption giving a corrected 99th percentile of about 1.27 s against about 6.04 ms uncorrected. The technique "needs a plan" giving each request's intended start; constant rate makes that simple [wrk2].
- **Aeron rig:** passes the *scheduled* timestamp (`timestampNs`, advanced by `sendIntervalNs`) into each message, not the actual send time. When behind schedule it does not skip ahead, so late bursts go out back to back (from the control flow, not a stated comment) [aeron-rig].
- **Chronicle (2017):** each message carries the time it should have been sent, "to avoid co-ordinated omission" [chronicle-2017]. JLBH exposes `accountForCoordinatedOmission(...)` and defines the problem as measurements that "ignore the time requests wait to be serviced" [jlbh].
- **Post-hoc correction (HdrHistogram):** `recordValueWithExpectedInterval()` adds synthetic values, decreasing linearly in steps of the expected interval, when a recorded value exceeds that interval. Its README example (10 ms interval, one 100 s pause) shows about 99.99% of raw results at 1 ms or below, but about 50% corrected. The two data sets differ "only if at least one value recorded ... was greater than" the expected interval [hdr-readme]. HdrHistogram_c has `hdr_record_corrected_value` [hdr-c].
- **Disruptor:** waits 1 us after each injection but does not say whether latency is measured from the scheduled or the actual injection time [disruptor-wiki].

## 4. Percentiles, histograms, and sample counts

### Which statistics the sources publish

| Source | Statistics | Samples |
|---|---|---|
| Disruptor paper, Table 4 | min, mean, "99% observations less than", "99.99% observations less than", max | 50 million events [disruptor-paper] |
| Disruptor wiki | mean, 99%, 99.99% | 50 million [disruptor-wiki] |
| wrk2 `--latency` | 50, 75, 90, 99, 99.9, 99.99, 99.999, 100th, plus the full spectrum; `--u_latency` adds the uncorrected one | rate x duration [wrk2] |
| Aeron benchmarks | HdrHistogram percentile distribution per run; `.hdr` files merged across runs | rate x 10 one-second iterations by default [aeron-readme] [aeron-config] |
| JLBH sample output | 50, 90, 99, worst, per run, with a "% Variation" column across runs | not stated [jlbh] |
| Chronicle 2017 | the throughput at which the 99%ile (and 99.9%ile) stays under 10 us | 20 million messages [chronicle-2017] |
| SIX X-stream INET | average, 95% within, 99% within | 20,000 OUCH orders/s from 80 clients; duration not stated [six-inet] |
| Turquoise 2010 (press) | average; "99.9% of all customer orders ... within 400 microseconds" | two and a half weeks of production [tradenews-2010] |
| This repo | median, p10, p90, min, max, CV of per-run throughput | 90 runs per row at standard size; percentiles above p90 not given because p99 "would rest on less than one run" [repo-bench] |

### What Tene says about the statistics

- "Don't use or derive from std. deviation." "Always measure Max time." "Measure %`iles. Lots of them." "Standard Deviation and application latency should never show up on the same page..." "If you haven't stated percentiles and a Max, you haven't specified your requirements" [tene-2012].
- Latency is "strongly multi-modal" and does not "look anything like a normal distribution" [tene-2012].
- "You can't average percentiles. Period." [lt-avg]. Omitting the max means "you are invariably ignoring a system requirement, and hiding the fact that you are ignoring it" [lt-max].
- For "low latency" trading he frames the requirement as a good median "But we also need a reasonable Max, or 99.99%`ile" [tene-2012].

### HdrHistogram

- Purpose: "recoding [sic] histograms of value measurements in latency and performance sensitive applications" [hdr-site].
- Range and precision are set by `highestTrackableValue` and `numberOfSignificantValueDigits`; with 3 digits, "Value quantization within the range will thus be no larger than 1/1,000th (or 0.1%) of any value". Tracking up to 1 hour (3,600,000,000 us) at 3 digits takes about 185 KB, fixed, whatever the sample count [hdr-readme].
- Layout is log-linear: buckets with "exponentially increasing bucket value ranges" (the exponent) each hold a fixed number of "linear sub-buckets" (the mantissa) [hdr-readme] [tene-2012].
- Recording is constant time with "no allocation operations"; the plain `Histogram` is not synchronized; `Recorder` and `SingleWriterRecorder` support wait-free interval sampling [hdr-readme].
- The C port is "a subset of the functionality supported by the Java implementation": `hdr_init(min, max, sigfigs)`, `hdr_record_value`, `hdr_record_corrected_value`, `hdr_value_at_percentile`, `hdr_percentiles_print` (CLASSIC or CSV), serialization, and an interval recorder; atomic or concurrent histograms are listed as unlikely to be implemented [hdr-c].
- Aeron's benchmarks build `new Histogram(HOURS.toNanos(1), 3)`, or a `SingleWriterRecorder(3)` when logging intervals, and report in microseconds by default [aeron-hist] [aeron-config].

## 5. Warm-up and run-to-run variance

- **Aeron:** 10 warm-up iterations of one second at 10,000 messages/s by default; "warmup iterations results will be discarded" (the rig resets the histograms after warm-up) [aeron-config] [aeron-rig]. Its README aggregates five runs into one histogram and says nothing about variance [aeron-readme].
- **JLBH:** "A rule of thumb is to warm up for about 30% of the run iterations", overridable; the sample output shows three runs with a % Variation column of about 11 to 18% [jlbh].
- **wrk2:** calibration extended "to 10 seconds (from wrk's 0.5 second)"; runs shorter than 10-20 s "may not present useful information"; latencies are accurate to "+/- ~1 msec" because of OS sleep behavior [wrk2].
- **Disruptor:** throughput is "the best of 3 runs when processing 500 million messages", and "Results can vary substantially across different JVM executions" [disruptor-paper]. The wiki says JVM configuration, Hyper-Threading, C-states, SMIs, and core binding "can make a significant difference to latency results"; the latency test used no binding [disruptor-wiki].
- **Chronicle 2017:** warm-up, run count, and tuning not stated; the 2016 and 2017 results come from different code and setups [chronicle-2017].
- **SIX:** "measured with a production configuration"; warm-up and run count not stated [six-inet].
- **This repo:** one warm-up run per workload and kind, recorded but excluded; every run on a fresh book; CV reported per row [repo-readme]. Session medians of the large-book workloads differed by 23 to 48% and throughput shifted partway through a session [repo-bench]. Whole-session A/B alternation measured an identical binary 9% faster because it ran second, so changes are judged by `benchmarks/compare_ab.py`: 16 ABBA-ordered pairs of short runs, the median ratio with a bootstrap 95% interval, and a measured noise floor of about plus or minus 3% [repo-opt] [repo-exp].

## 6. Claims made and refused

- **Disruptor:** "We believe that this is the highest performance mechanism for such data exchange"; "latency remains almost flat until saturation occurs of the memory sub-system". Every figure names its CPU, OS, and JVM, and is compared with `ArrayBlockingQueue` on the same machine [disruptor-paper].
- **Tene:** "Measuring throughput without latency behavior is [usually] meaningless"; sustainable throughput is "The throughput achieved while safely maintaining service levels" [tene-2012].
- **SIX:** headline "customers benefit from latency of 10.6 microseconds", which is the average OUCH round trip at the network boundary; the table adds 95% and 99% (18.4 us) but no higher percentile or max. The document disclaims liability for "completeness, accuracy, currentness" [six-inet].
- **Turquoise (press, 2010):** an average of 126 us via the native API and 196 us via FIX 5.0, and the claim "Our speeds are now double that of our nearest competitors ... on a like-for-like basis", with "like-for-like" undefined [tradenews-2010].
- **Chronicle 2017:** results are tied to 40-byte messages, one writer and one reader, and a 10 us target [chronicle-2017].
- **wrk2:** describes itself as "currently in experimental/development mode" [wrk2].
- **This repo:** "ns/command is 10^9 / median: the mean cost per command within a median run. It is not the latency of any single command." Per-command percentiles "are not reported, because they were not measured". The numbers are "not a comparison with any production exchange", and nothing supports "low latency", "microsecond", "exchange grade", or tail-latency claims [repo-bench]. `docs/OPTIMIZATION.md` adds "Only the ratios are the claim" [repo-opt].

## 7. What the repo's methodology already fixes

From `benchmarks/README.md` [repo-readme], `docs/BENCHMARKS.md` [repo-bench], `docs/OPTIMIZATION.md` [repo-opt], and `docs/OPTIMIZATION-EXPERIMENTS.md` [repo-exp]:

- Workloads are generated in full, deterministically, before timing; generation is never timed. The `core` kind calls `submitOrder` and `cancelOrder` directly; `replay` adds in-memory CSV parsing and the replay engine, with no disk I/O.
- The clock is `steady_clock`, read once before and once after each run of 10,000 to 1,000,000 commands. A standard run lasts at least 15 ms against a 100 ns tick.
- Unoptimized builds (no `NDEBUG`, automatic invariant checks, ASan, UBSan) refuse to run without `--allow-unoptimized`.
- One thread, no core pinning, no raised priority. CPU, memory, OS, flags, and machine conditions are written down by hand.
- Medians, not means; p10 and p90 only where 90 runs support them; CV as the noise check.
- Changes are judged by paired, interleaved ABBA runs with a bootstrap interval, against a measured noise floor.
- Throughput only, with explicit refusals of latency, tail, and cross-system claims. The stated reason per-command timing was not attempted: commands of about 150 to 1,100 ns are "comparable to the 100 ns tick of steady_clock on this machine and to the cost of reading it".

Not covered by the existing harness, per the same documents: per-command or per-message latency, any histogram, any load generator with an arrival schedule, and any queue hand-off or round trip.

## Gaps and source caveats

- Tene's QCon SF 2015 slides (InfoQ) returned HTTP 403 or an HTML page, and the talk transcript (Amara) returned 403. Tene's material here comes from the QCon SF 2012 slide PDF, his 2013 mailing-list thread, and his #LatencyTipOfTheDay blog. The 2012 deck has no explicit rule for samples per percentile.
- **CME:** no CME-published order-entry latency figure or definition was found. A 2020 Globex notice that search results describe as mentioning a 3 us processing overhead timed out and was not read.
- **Nasdaq (US):** no Nasdaq-published INET latency figure was found. The SIX document covers Nasdaq's X-stream INET as deployed at SIX, May 2025 edition. A-Team Insight's article on SIX's 2010 rollout was seen only in search results and is not cited.
- **LSE:** only 2010 trade-press figures for Turquoise were read; WatersTechnology's article was truncated by a paywall, and The Trade News article does not define its measurement points.
- The Intel SDM wording was read from an unofficial mirror.
- The libstdc++ source was read from the GitHub mirror of GCC, since gcc.gnu.org refused the request.

## Sources

[tene-2012]: https://qconsf.com/sf2012/dl/qcon-sanfran-2012/slides/GilTene_HowNotToMeasureLatency.pdf
[tene-co]: https://groups.google.com/g/mechanical-sympathy/c/icNZJejUHfE
[lt-avg]: https://latencytipoftheday.blogspot.com/2014/06/latencytipoftheday-you-cant-average.html
[lt-max]: https://latencytipoftheday.blogspot.com/2014/06/latencytipoftheday-if-you-are-not.html
[hdr-site]: http://hdrhistogram.github.io/HdrHistogram/
[hdr-readme]: https://github.com/HdrHistogram/HdrHistogram/blob/master/README.md
[hdr-c]: https://github.com/HdrHistogram/HdrHistogram_c/blob/main/README.md
[wrk2]: https://github.com/giltene/wrk2/blob/master/README.md
[disruptor-paper]: https://lmax-exchange.github.io/disruptor/disruptor.html
[disruptor-wiki]: https://github.com/LMAX-Exchange/disruptor/wiki/Performance-Results
[aeron-readme]: https://github.com/aeron-io/benchmarks/blob/master/README.md
[aeron-rig]: https://github.com/aeron-io/benchmarks/blob/master/benchmarks-api/src/main/java/io/aeron/benchmarks/LoadTestRig.java
[aeron-config]: https://github.com/aeron-io/benchmarks/blob/master/benchmarks-api/src/main/java/io/aeron/benchmarks/Configuration.java
[aeron-hist]: https://github.com/aeron-io/benchmarks/blob/master/benchmarks-api/src/main/java/io/aeron/benchmarks/PersistedHistogram.java
[jlbh]: https://github.com/OpenHFT/JLBH/blob/ea/README.adoc
[chronicle-2017]: https://vanilla-java.github.io/2017/02/06/Improving-percentile-latencies-in-Chronicle-Queue.html
[six-inet]: https://www.six-group.com/dam/download/the-swiss-stock-exchange/trading/trading-platform/x-stream-inet-performance-measurement-details.pdf
[tradenews-2010]: https://www.thetradenews.com/turquoise-lays-claim-to-lowest-latency-trading-platform/
[ms-qpc]: https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
[msvc-chrono]: https://github.com/microsoft/STL/blob/main/stl/inc/__msvc_chrono.hpp
[libstdcxx-chrono]: https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/src/c%2B%2B11/chrono.cc
[man-clock]: https://man7.org/linux/man-pages/man2/clock_gettime.2.html
[man-vdso]: https://man7.org/linux/man-pages/man7/vdso.7.html
[kernel-tsc]: https://www.kernel.org/doc/html/latest/virt/kvm/x86/timekeeping.html
[intel-sdm-mirror]: https://xem.github.io/minix86/manual/intel-x86-and-64-manual-vol3/o_fe12b1e2a880e0ce-615.html
[repo-readme]: ../../benchmarks/README.md
[repo-bench]: ../BENCHMARKS.md
[repo-opt]: ../OPTIMIZATION.md
[repo-exp]: ../OPTIMIZATION-EXPERIMENTS.md

- Gil Tene, "How Not to Measure Latency", QCon San Francisco 2012 slides: [tene-2012]
- Gil Tene, "Coordinated Omission", mechanical-sympathy mailing list, 3 August 2013: [tene-co]
- Gil Tene, #LatencyTipOfTheDay posts, June 2014: [lt-avg], [lt-max]
- HdrHistogram project page, Java README, and C port README: [hdr-site], [hdr-readme], [hdr-c]
- wrk2 README: [wrk2]
- LMAX Disruptor technical paper and Performance Results wiki: [disruptor-paper], [disruptor-wiki]
- Aeron benchmarks README and source: [aeron-readme], [aeron-rig], [aeron-config], [aeron-hist]
- JLBH README and Chronicle Queue percentile post (Peter Lawrey, 2017): [jlbh], [chronicle-2017]
- SIX Swiss Exchange, X-stream INET Performance Measurement Details, May 2025: [six-inet]
- The Trade News, Turquoise latency, 20 October 2010: [tradenews-2010]
- Microsoft, Acquiring high-resolution time stamps: [ms-qpc]
- MSVC STL `__msvc_chrono.hpp`; libstdc++ `chrono.cc`: [msvc-chrono], [libstdcxx-chrono]
- Linux man pages `clock_gettime(2)` and `vdso(7)`; kernel KVM timekeeping doc: [man-clock], [man-vdso], [kernel-tsc]
- Intel SDM Vol. 3, Invariant TSC (mirror): [intel-sdm-mirror]
- This repo: [repo-readme], [repo-bench], [repo-opt], [repo-exp]
