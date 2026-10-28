# How single- and multi-producer queues hand commands between threads

Research for [issue #21](https://github.com/ThomasStatic/order-book/issues/21) on the wayfinder map, [#37](https://github.com/ThomasStatic/order-book/issues/37). Read 2026-10-28.

This note records what five designs say about handing items between threads: the LMAX Disruptor (paper, user guide, and Java source), rigtorp's `SPSCQueue` and `MPMCQueue`, folly's `ProducerConsumerQueue`, and Boost.Lockfree (`spsc_queue`, `queue`, and the rationale page). It also records what C++20 `std::atomic` guarantees and how the memory orders map to x86 and ARM instructions. It states facts and recommends no design.

Citations are reference links (for example [spsc-src]), resolved in [Sources](#sources). "Not stated" means the pages read say nothing about it. Nothing is filled in from memory.

## Facts the decisions wait on

- Every bounded ring in this survey is an array of slots indexed by a monotonically increasing counter (or a pair of counters) masked or reduced modulo the capacity. The SPSC rings publish one index per side with a release store and read the other side's index with an acquire load; nothing else is needed for one producer and one consumer [spsc-src] [folly-src] [boost-spsc-src] [paper-html].
- Full and empty are never signalled; the side that cares re-reads the other side's index (or, in the Disruptor and `MPMCQueue`, a per-slot availability flag or turn counter) and finds out for itself [spsc-src] [folly-src] [mpmc-src] [multi-seq].
- Multi-producer designs add one atomic read-modify-write per operation (`fetch_add` or CAS on a ticket counter, Disruptor `cursor.getAndAdd(n)`) and a per-slot marker so that a consumer can tell which claimed slots have actually been written [mpmc-src] [multi-seq] [boost-queue-src].
- No design in this survey blocks inside the queue by default. rigtorp and folly spin (bare loops, no yield); Boost.Lockfree returns `false`; the Disruptor delegates waiting to a `WaitStrategy`, whose default is a lock plus condition variable and whose alternatives are spin, spin-then-yield, and spin-yield-park [spsc-src] [mpmc-src] [folly-doc] [boost-spsc-src] [guide] [blocking] [sleeping] [yielding] [busyspin]. C++20 adds `std::atomic::wait`/`notify_one`/`notify_all` as a standard blocking primitive [cpp-wait].
- All five pad or align the two shared indices to separate cache lines. The constant differs: rigtorp uses `std::hardware_destructive_interference_size` when `__cpp_lib_hardware_interference_size` is defined, else 64; folly uses the standard value when available, else 128 on x86 and 64 on ARM/s390x; Boost uses 64 except 128 on PowerPC and Apple ARM and 256 on s390; the Disruptor pads each `Sequence` with 56 bytes on each side [spsc-src] [mpmc-src] [folly-align] [boost-prefix] [sequence]. GCC warns on any use of the standard constant by default because its value follows `-mtune` and is an ABI hazard [gcc-warn]; libc++ only implements it as of LLVM 19 [libcxx].
- On x86 an acquire load and a release store are plain `MOV`s and only a `seq_cst` store or RMW costs a locked instruction or `MFENCE`; on AArch64 they are `LDAR`/`STLR`, and on ARMv7 they need `DMB ISH` [mappings] [llvm-atomics] [cpp-order].
- The only measured comparison against a lock-and-condition-variable queue in these sources is the Disruptor paper's: on a 2011 laptop, 1P-1C throughput of 22.4M ops/s against `ArrayBlockingQueue`'s 4.1M ops/s, and mean three-stage-pipeline latency of 52 ns against 32,757 ns [paper-html] [wiki-perf]. rigtorp's README measures only against other lock-free queues (`boost::lockfree::spsc_queue`, `folly::ProducerConsumerQueue`) [spsc-readme]. Boost.Lockfree says lock-free structures "are not necessarily the best choice for every use case" and asks the reader to benchmark [boost-intro].
- The Disruptor batches by letting a consumer process every sequence up to the published cursor in one pass; it multicasts every event to every handler and expresses dependencies as sequence barriers on one ring; it returns nothing to the producer. A handler that wants to send results on either writes them into the same pre-allocated event for a downstream handler or publishes to another ring [paper-html] [batch-proc] [guide].

## Comparison

| | LMAX Disruptor | rigtorp `SPSCQueue` | rigtorp `MPMCQueue` | folly `ProducerConsumerQueue` | Boost `spsc_queue` | Boost `queue` |
|---|---|---|---|---|---|---|
| Producers / consumers | 1 or N producers; N consumers, each sees every event (multicast) [guide] | 1 / 1 [spsc-readme] | N / N [mpmc-readme] | 1 / 1 [folly-doc] | 1 / 1 [boost-intro] | N / N [boost-intro] |
| Storage | Pre-allocated array of events, size a power of 2, `bufferSize + 2 * 32` slots with 32 padding slots each end [ring-src] | Array of `capacity + 1` slots plus `kPadding` slots each end; any capacity [spsc-src] | Array of `capacity + 1` slots, each slot cache-line aligned with its own `turn` counter [mpmc-src] | Array of `size` slots, rounded up to a power of two [folly-src] | Array of `max_elements + 1` slots [boost-spsc-src] | Linked list of nodes from a freelist; never returns memory to the OS [boost-rationale] |
| Index representation | 64-bit sequences, slot = `sequence & indexMask` [ring-src] | Two `size_t` indices that wrap at `capacity_` [spsc-src] | Two `size_t` tickets; `turn(i) = i / capacity_`, `idx(i) = i % capacity_` [mpmc-src] | Two ever-increasing `uint64_t` counters masked on access [folly-src] | Two `size_t` indices that wrap; `(i + 1) & mask` when a power of two, else a subtraction loop [boost-spsc-src] | Tagged head and tail pointers (ABA tag) [boost-queue-src] [boost-rationale] |
| Full detection | Producer compares `wrapPoint = next - bufferSize` with the minimum gating (consumer) sequence [single-seq] [multi-seq] | `nextWriteIdx == readIdxCache_`, refreshed by an acquire load of `readIdx_` [spsc-src] | Slot `turn != turn(head) * 2`; if the ticket counter has not moved, report full [mpmc-src] | `currentWrite - producer_.remoteIndexCache >= size` [folly-src] | `next == read_index_.load(acquire)` [boost-spsc-src] | `bounded_push` returns false when the freelist is exhausted [boost-queue-src] |
| Empty detection | Consumer waits on the barrier for `cursor >= sequence`, then `getHighestPublishedSequence` [barrier] [multi-seq] | `readIdx == writeIdxCache_`, refreshed by an acquire load of `writeIdx_` [spsc-src] | Slot `turn != turn(tail) * 2 + 1` [mpmc-src] | `currentRead >= consumer_.remoteIndexCache` [folly-src] | `write_index_.load(acquire) == read_index` [boost-spsc-src] | Head's `next` is null [boost-queue-src] |
| Waiting when full / empty | `WaitStrategy` for consumers; producers `LockSupport.parkNanos(1L)` in a loop [single-seq] [multi-seq] [guide] | `push`/`emplace`/`pop` busy-spin, `try_*` and `front` return at once [spsc-src] | Bare `while` spin, no yield [mpmc-src] | None; all operations wait-free and return `false`; caller spins [folly-doc] | None; `push`/`pop` return `false` [boost-spsc-src] | None; returns `false` [boost-queue-src] |
| Cache-line constant | 56 padding bytes each side of a `Sequence` value; 32 pad slots each end of the entry array [sequence] [ring-src] | `std::hardware_destructive_interference_size` if available, else 64 [spsc-src] | Same, "not defined on Apple platforms" per source [mpmc-src] | `std::` value if available, else 128 (x86) or 64 (ARM, s390x) [folly-align] | 64, or 128 (PowerPC, Apple ARM), or 256 (s390) [boost-prefix] | Same constant; `tail_` and `node::next` aligned, `head_` not [boost-queue-src] |
| Publish / consume ordering | `Sequence.set` = release fence then plain write; `get` = plain read then acquire fence; multi-producer availability flags `setRelease`/`getAcquire` [sequence] [multi-seq] | Own index relaxed; other index acquire; publish release [spsc-src] | `fetch_add`/CAS default `seq_cst`; slot `turn` load acquire, store release [mpmc-src] | Own index relaxed; other index acquire; publish release [folly-src] | Own index relaxed; other index acquire; publish release [boost-spsc-src] | Loads acquire; CAS default `seq_cst` [boost-queue-src] |
| Progress guarantee claimed | Not stated as lock-/wait-free; "just memory barriers" for one producer [paper-html] | "wait-free and lock-free" [spsc-readme] | Not stated in README [mpmc-readme] | "wait-free" [folly-doc] | "wait-free" [boost-intro] | "lock-free" on head, tail, freelist; `push` "may not be lock-free" [boost-queue-src] |
| Element requirements | Pre-allocated mutable events via `EventFactory` [guide] | `pop` needs nothrow-destructible `T` [spsc-readme] | Not stated | Not stated | Default constructor, copyable or movable [boost-spsc-src] | Trivial copy assignment and trivial destructor [boost-queue-src] |
| Published numbers vs mutex+condvar | Yes (Tables 2 and 3 in the paper; see section 7) [paper-html] | Only vs other lock-free queues [spsc-readme] | None [mpmc-readme] | None [folly-doc] | None; "benchmark" advice only [boost-intro] | Same |

## 1. Bounded ring design

**Disruptor.** Entries are pre-allocated as an array filled by an `EventFactory` so that no memory is allocated per message [paper-html] [ring-src]. The size must be a power of two ("bufferSize must be a power of 2") so that the slot is `sequence & indexMask` instead of a remainder [ring-src] [paper-html]. The array is allocated as `bufferSize + 2 * BUFFER_PAD` with `BUFFER_PAD = 32`, and `elementAt` reads `entries[BUFFER_PAD + (int)(sequence & indexMask)]`, so the first and last live slots never share a cache line with neighbouring heap objects [ring-src]. Sequences are 64-bit `long`s that never wrap in practice; `Sequence.INITIAL_VALUE` is -1 [sequence].

**rigtorp SPSCQueue.** A ring of `capacity + 1` slots, "one slack element", so full and empty are distinguishable; `capacity()` reports the requested value and any non-power-of-two capacity is allowed. Indices wrap by comparison (`if (nextWriteIdx == capacity_) nextWriteIdx = 0`). `kPadding = (kCacheLineSize - 1) / sizeof(T) + 1` slots pad each end of the slot array, and element access is `slots_[idx + kPadding]` [spsc-src] [spsc-readme].

**rigtorp MPMCQueue.** A ring of `capacity + 1` slots (the extra one pads the last slot). Each `Slot` is `alignas(hardwareInterferenceSize)` and holds an atomic `turn` and raw storage. `head_` and `tail_` are tickets that only grow; `turn(i) = i / capacity_` and `idx(i) = i % capacity_`. A slot is writable in round `t` when its `turn == t * 2` and readable when `turn == t * 2 + 1` [mpmc-src] [mpmc-readme].

**folly ProducerConsumerQueue.** `size` in [1, 2^31] is rounded up to a power of two (`mask = nextPowTwo(size) - 1`). Indices are 64-bit counters that never wrap and are masked only when indexing. The class comment says the design distinguishes full from empty "without reserving a slot" and that "the classic version of this design capped usable capacity at size - 1" [folly-src]. The library documentation still says it "allocates that many cells of sizeof(T)" [folly-doc].

**Boost spsc_queue.** A ring of `max_elements + 1` slots (one reserved). `next_index` is `(i + 1) & buffer_mask_` for a compile-time power-of-two capacity and otherwise a subtraction loop guarded by an inline-asm barrier so the compiler cannot turn it into a division. Ranged `push`, `pop`, `consume_one`, and `consume_all` move several elements per index update [boost-spsc-src].

**Boost queue.** Not a ring: a Michael-Scott linked list whose nodes come from a freelist and are never returned to the OS, because freeing may block and "most memory reclamation algorithms are patented". `fixed_sized` stores nodes in an array addressed by 16-bit index, which also sidesteps double-width CAS [boost-rationale] [boost-queue-src].

## 2. How full and empty are signalled

Nothing is signalled in any of the five designs; each side discovers the state by reading.

- **SPSC rings (rigtorp, folly, Boost).** The producer owns the write index and the consumer owns the read index. Full is "next write index equals read index" (rigtorp, Boost) or "write minus cached read >= size" (folly); empty is "read index equals write index" (rigtorp, Boost) or "read >= cached write" (folly) [spsc-src] [folly-src] [boost-spsc-src].
- **Cached indices.** rigtorp and folly keep a local copy of the other side's index (`readIdxCache_`/`writeIdxCache_`, `remoteIndexCache`) and reload the shared atomic only when the cache says full or empty. The README says this "increases throughput by reducing the amount of cache coherency traffic" [spsc-readme] [spsc-src] [folly-src]. Boost's `spsc_queue` reads the other side's index on every push and pop [boost-spsc-src].
- **MPMCQueue.** There is no explicit full or empty test. `try_emplace` loads `head_` (acquire), checks the slot's `turn`, and claims with `compare_exchange_strong(head, head + 1)`; if the turn is wrong it reloads `head_` and returns `false` only if the ticket has not moved, otherwise retries. `try_pop` mirrors this on `tail_` [mpmc-src].
- **Disruptor, single producer.** `next(n)` computes `wrapPoint = nextSequence - bufferSize` and compares it with `cachedValue`, the last known minimum of the gating sequences; only when `wrapPoint > cachedGatingSequence` does it re-read every gating sequence with `Util.getMinimumSequence` [single-seq]. Consumers are gated through a `SequenceBarrier` that waits until `cursorSequence.get() >= sequence` [barrier].
- **Disruptor, multiple producers.** The cursor is advanced by `cursor.getAndAdd(n)` before the slot is written, so the cursor alone cannot tell a consumer what is readable. An `int[] availableBuffer`, initialised to -1, records the lap number `(int)(sequence >>> indexShift)` per slot; `publish` stores it with `setRelease` and `isAvailable` reads it with `getAcquire`. `getHighestPublishedSequence` scans forward from the consumer's position and returns `sequence - 1` at the first unavailable slot. The javadoc gives the reason: "The prime reason is to avoid a shared sequence object between publisher threads" [multi-seq].

## 3. Backpressure and blocking strategies

**rigtorp.** `push`, `emplace`, and `pop` are bare `while` loops; no yield, pause, or back-off. `try_*` variants return `false`; `front()` returns `nullptr` when empty. The README example spins with `while (!q.front());` [spsc-src] [mpmc-src] [spsc-readme].

**folly.** "All of these operations are wait-free." `write` and `read` return `false` on full or empty and the example spins ("spin until the queue has room"). The documentation advises sizing the queue so that it is unlikely to fill or stay empty for long [folly-doc].

**Boost.Lockfree.** `push`/`pop` return `false`; there is no blocking API. The introduction notes that blocking can creep in through atomics emulated with spinlocks, OS memory allocation, and exception handling, and that lock-free structures "are not necessarily the best choice for every use case" [boost-intro] [boost-spsc-src].

**Disruptor producers.** When the ring is full, both sequencers loop `LockSupport.parkNanos(1L)` with the comment "TODO, should we spin based on the wait strategy?" `tryNext` throws `InsufficientCapacityException` instead of parking [single-seq] [multi-seq] [ring-src].

**Disruptor consumers.** Waiting is a pluggable `WaitStrategy` [guide]:

| Strategy | Behaviour (from source) | Guide's characterisation |
|---|---|---|
| `BlockingWaitStrategy` (default) | `synchronized (mutex) { while (cursorSequence.get() < sequence) mutex.wait(); }`, then spins with `Thread.onSpinWait()` on the dependent sequence; `signalAllWhenBlocking` does `mutex.notifyAll()` [blocking] | Slowest, most conservative with CPU, most consistent across deployments [guide] |
| `SleepingWaitStrategy` | Spin while `counter > 100`, then `Thread.yield()` while `counter > 0`, then `LockSupport.parkNanos(100)`; `DEFAULT_RETRIES = 200`. "Latency spikes can occur after quiet periods" [sleeping] | For cases where latency is not required but producer impact must be minimal, such as asynchronous logging; `parkNanos(1)` pauses roughly 60 us on typical Linux [guide] |
| `YieldingWaitStrategy` | Spin `SPIN_TRIES = 100` times, then `Thread.yield()` on each pass; "will use 100% CPU" [yielding] | Recommended for very high performance when handler threads are fewer than logical cores [guide] |
| `BusySpinWaitStrategy` | `while ((availableSequence = dependentSequence.get()) < sequence) { barrier.checkAlert(); Thread.onSpinWait(); }`; "avoid syscalls which can introduce latency jitter" [busyspin] | Highest performing, only when handler threads are fewer than physical cores with hyper-threading off [guide] |

Every producer `publish` calls `waitStrategy.signalAllWhenBlocking()`, which is a no-op for the spinning strategies and a `notifyAll` for the blocking one [single-seq] [multi-seq] [blocking]. The paper summarises the trade: a condition variable "is a point of contention and only to be used when CPU resource is more important than latency or throughput"; polling the cursor "could be done with or without a thread yield by trading CPU resource against latency" [paper-pdf].

**C++20 standard primitive.** `std::atomic<T>::wait(old, order)` blocks until `notify_one` or `notify_all` and the value differs from `old`; it is "guaranteed to return only if value has changed, even if underlying implementation unblocks spuriously"; `order` may not be `release` or `acq_rel`; the comparison is bitwise; "This form of change-detection is often more efficient than simple polling or pure spinlocks" [cpp-wait] [cpp-atomic]. None of the surveyed C++ libraries use it.

## 4. False sharing and cache-line padding

- The paper: caches are organised in lines "typically 32-256 bytes in size, the most common cache-line being 64 bytes"; two variables in one line written by different threads suffer "false sharing"; independent but concurrently written variables must not share a line. Head, tail, and size counters in conventional queues "generally occupy the same cache-line" [paper-pdf].
- Disruptor source: `Sequence` is `LhsPadding` (56 `byte`s) -> `Value` (one `long`) -> `RhsPadding` (56 `byte`s) -> `Sequence`; `SingleProducerSequencer` and `RingBuffer` repeat the same byte padding above and below their fields; the entry array has 32 pad slots at each end [sequence] [single-seq] [ring-src].
- rigtorp: `alignas(kCacheLineSize)` on `writeIdx_`, `readIdxCache_`, `readIdx_`, and `writeIdxCache_` (four lines); `static_assert(alignof(SPSCQueue<T>) == kCacheLineSize)` and `sizeof >= 3 * kCacheLineSize`; `kCacheLineSize` is `std::hardware_destructive_interference_size` under `#ifdef __cpp_lib_hardware_interference_size`, else 64 [spsc-src]. `MPMCQueue` aligns each slot's `turn`, `head_`, and `tail_` the same way and asserts `tail_` sits exactly one interference size after `head_` [mpmc-src].
- folly: `alignas(hardware_destructive_interference_size) Side producer_; alignas(...) Side consumer_;` where each `Side` holds the atomic `localIndex` and the plain `remoteIndexCache` [folly-src]. folly's own constant is `(kIsArchArm || kIsArchS390X) ? 64 : 128`, with the comment "Microbenchmarks indicate that pairs of cache lines also see destructive interference under heavy use of atomic operations, as observed for atomic increment on Sandy Bridge"; it defers to the standard constant when `__cpp_lib_hardware_interference_size` is defined [folly-align].
- Boost: `alignas(detail::cacheline_bytes)` on `storage_`, `write_index_`, and `read_index_`; `cacheline_bytes` is 64, or 128 on PowerPC and Apple ARM, or 256 on s390; it does not use the standard constant [boost-spsc-src] [boost-prefix].
- The standard constant: `std::hardware_destructive_interference_size` (C++17, `<new>`) is the "Minimum offset between two objects to avoid false sharing. Guaranteed to be at least alignof(std::max_align_t)"; feature macro `__cpp_lib_hardware_interference_size` = 201703L. cppreference's example reports 64 on x86-64 and measures a 6.16x slowdown for two atomics on one line versus two lines [cpp-interference]. Implementations: libstdc++ defines it as `__GCC_DESTRUCTIVE_SIZE` [libstdcxx-new]; GCC 12 added it with `-Winterference-size` [gcc12]; the warning is on by default because the value follows `-mtune`, "So all translation units that depend on ABI compatibility for the use of these variables must be compiled with the same -mtune (or -mcpu)", and a library header should "probably not use the hardware interference size variables at all" [gcc-warn]; GCC's parameter help text calls it "Typically the L1 cache line size, but can be larger" and "strongly discouraged" in public ABIs [gcc-params]. MSVC documents both constants as implementation-defined with the same `keep_apart` example [msvc-new]. libc++ marks P0154R1 "Complete" with "The required macros are only implemented as of LLVM 19" [libcxx].

## 5. Acquire/release pairs each design relies on

| Design | Producer side | Consumer side | Source |
|---|---|---|---|
| rigtorp SPSCQueue | `writeIdx_.load(relaxed)` (own); `readIdx_.load(acquire)` when cache says full; construct; `writeIdx_.store(next, release)` | `readIdx_.load(relaxed)` (own); `writeIdx_.load(acquire)` when cache says empty; destroy; `readIdx_.store(next, release)` | [spsc-src] |
| folly | `producer_.localIndex` relaxed load; `consumer_.localIndex` acquire load on refresh; release store to `producer_.localIndex` | Mirror image; `sizeGuess` is two relaxed loads, "Provides no memory ordering" | [folly-src] |
| Boost spsc_queue | `write_index_.load(relaxed)`, `read_index_.load(acquire)`, `write_index_.store(release)` | `write_index_.load(acquire)`, `read_index_.load(relaxed)`, `read_index_.store(release)`; `empty()` is two relaxed loads | [boost-spsc-src] |
| rigtorp MPMCQueue | `head_.fetch_add(1)` (default `seq_cst`); spin on `slot.turn.load(acquire)`; construct; `slot.turn.store(turn*2+1, release)` | `tail_.fetch_add(1)`; spin on `slot.turn.load(acquire)`; move out; `slot.turn.store(turn*2+2, release)`; `size()` relaxed | [mpmc-src] |
| Boost queue | `tail_`, `next` loads acquire; `compare_exchange_weak`/`strong` default `seq_cst` | `head_`, `tail_`, `next` loads acquire; CAS default | [boost-queue-src] |
| Disruptor | `Sequence.set`: `VarHandle.releaseFence()` then plain write ("a Store/Store barrier between this write and any previous store"); `setVolatile` adds `fullFence()` ("a Store/Load barrier") and is used before a producer re-reads gating sequences; multi-producer `availableBuffer` written with `setRelease` | `Sequence.get`: plain read then `VarHandle.acquireFence()`; `availableBuffer` read with `getAcquire` | [sequence] [single-seq] [multi-seq] |

The pattern common to the SPSC rings is one release store on the publishing index paired with one acquire load of it on the other side; cppreference's rule is that when a release store is read by an acquire load of the same atomic, the store "synchronizes-with" the load and "All memory writes (including non-atomic and relaxed atomic) that happened-before the atomic store in thread A also happened-before the atomic load in thread B" [cpp-order]. The element construction before the release store and the destruction before the consumer's release store are the non-atomic writes being ordered [spsc-src].

## 6. What C++20 std::atomic guarantees portably

- `memory_order_relaxed`: "only this operation's atomicity is guaranteed"; relaxed operations "only guarantee atomicity and modification order consistency". `acquire`: "no reads or writes in the current thread can be reordered before this load. All writes in other threads that release the same atomic variable are visible in the current thread". `release`: "no reads or writes in the current thread can be reordered after this store". `acq_rel`: both, for read-modify-write. `seq_cst`: acquire and release "plus a single total order exists in which all threads observe all modifications in the same order" [cpp-order].
- `memory_order_consume` is "temporarily discouraged" (C++17 to C++23) and deprecated in C++26; "no known production compilers track dependency chains: consume operations are lifted to acquire operations" [cpp-order].
- `std::atomic<T>` requires a trivially copyable, copy- and move-constructible and assignable `T`; `is_always_lock_free` (C++17) "indicates that the type is always lock-free"; `atomic_signed_lock_free`/`atomic_unsigned_lock_free` are the integral types "for which waiting/notifying is most efficient"; `wait`, `notify_one`, `notify_all` are C++20 [cpp-atomic].
- Platform cost, per cppreference: on "strongly-ordered systems (x86, SPARC TSO, IBM mainframe, etc.) release-acquire ordering is automatic for the majority of operations. No additional CPU instructions are issued"; on "weakly-ordered systems (ARM, Itanium, PowerPC), special CPU load or memory fence instructions are used"; "Total sequential ordering requires a full memory fence CPU instruction on all multi-core systems" [cpp-order].
- Instruction mappings (Cambridge C/C++11 mappings page): x86 loads of every order are `MOV`; relaxed and release stores are `MOV`; a `seq_cst` store is `XCHG` (implicit `LOCK`) or `MOV; MFENCE`; the `seq_cst` fence is `MFENCE` and all other fences emit nothing. AArch64: relaxed load/store are `LDR`/`STR`; acquire and `seq_cst` loads are `LDAR`; release and `seq_cst` stores are `STLR`; CAS uses `LDAXR`/`STLXR` variants; the acquire fence is `DMB ISHLD` and the others `DMB ISH`. ARMv7: acquire load is `LDR; DMB ISH`, release store is `DMB ISH; STR`, `seq_cst` store is `DMB ISH; STR; DMB ISH`, every fence is `DMB ISH`, and 64-bit atomics without LPAE need an exclusive loop. The page warns against mixing the two x86 `seq_cst` conventions across compilers [mappings].
- LLVM's documentation agrees for x86: "all atomic loads generate a MOV", "SequentiallyConsistent stores generate an XCHG", "other stores generate a MOV", "SequentiallyConsistent fences generate an MFENCE", "other fences do not cause any code to be generated"; and for pre-v8 ARM: acquire, release, and `seq_cst` "require barrier instructions for every such operation" (`dmb`) [llvm-atomics].

## 7. Measured throughput and latency against a mutex-and-condition-variable queue

**Disruptor paper (2011), Table 2, ops/s, best of 3 runs over 500 million messages, Java 1.6.0_25 64-bit** [paper-html] [wiki-perf]:

| Topology | ABQ, Nehalem 2.8 GHz Win 7 | Disruptor | ABQ, Sandy Bridge 2.2 GHz Linux | Disruptor |
|---|---|---|---|---|
| Unicast 1P-1C | 5,339,256 | 25,998,336 | 4,057,453 | 22,381,378 |
| Pipeline 1P-3C | 2,128,918 | 16,806,157 | 2,006,903 | 15,857,913 |
| Sequencer 3P-1C | 5,539,531 | 13,403,268 | 2,056,118 | 14,540,519 |
| Multicast 1P-3C | 1,077,384 | 9,377,871 | 260,733 | 10,860,121 |
| Diamond 1P-3C | 2,113,941 | 16,143,613 | 2,082,725 | 15,295,197 |

`ArrayBlockingQueue` (ABQ) is `java.util.concurrent`'s lock-and-condition queue. The paper chose it over `ConcurrentLinkedQueue` "to ensure producers do not outpace consumers by creating back pressure" [paper-pdf].

**Latency, Table 3, three-stage pipeline, ns, Core i7-2720QM 2.2 GHz, Ubuntu 11.04, 1 us between injected events, 50 million events, no CPU binding** [paper-pdf] [wiki-perf]:

| | ArrayBlockingQueue | Disruptor |
|---|---|---|
| Min | 145 | 29 |
| Mean | 32,757 | 52 |
| 99% below | 2,097,152 | 128 |
| 99.99% below | 4,194,304 | 8,192 |
| Max | 5,069,086 | 175,567 |

"Profiling shows the use of locks and signalling via a condition variable are the main cause of latency for the ArrayBlockingQueue" [paper-pdf].

**Updated HTML edition of the paper (Table 3 there, AMD EPYC 9374F, OpenJDK 11.0.24, ops/s)** [paper-html]: Unicast ABQ 20,895,148 vs Disruptor 4 160,359,204; Pipeline 5,216,647 vs 101,317,122; Sequencer 3P-1C 18,791,340 vs 29,726,516; Multicast 2,355,379 vs 70,018,204; Diamond 3,433,665 vs 63,123,343. The user guide's own tables show roughly 80M ops/s single-producer against roughly 29M ops/s multi-producer and recommend the single-writer principle [guide].

**Cost-of-contention micro-experiment (Table 1), 500 million increments of a 64-bit counter, 2.4 GHz Westmere EP, Java** [paper-pdf] [paper-html]: single thread 300 ms; single thread with lock 10,000 ms; two threads with lock 224,000 ms; single thread with CAS 5,700 ms; two threads with CAS 30,000 ms; single thread with volatile write 4,700 ms.

**rigtorp SPSCQueue README, AMD Ryzen 9 3900X, two cores on one chiplet, `int` items** [spsc-readme]: throughput 362,723 ops/ms (SPSCQueue), 209,877 (`boost::lockfree::spsc`), 148,818 (`folly::ProducerConsumerQueue`); round-trip latency over two queues 133 ns, 222 ns, 147 ns. No mutex-based queue is in the comparison. `MPMCQueue`'s README has no benchmark ("Add benchmarks and compare to boost::lockfree::queue and others" is a TODO) [mpmc-readme]. folly claims only "very low synchronization overhead" [folly-doc].

## 8. How the Disruptor batches

- `BatchEventProcessor.processEvents` starts at `nextSequence = sequence.get() + 1`, calls `availableSequence = sequenceBarrier.waitFor(nextSequence)`, caps the batch at `endOfBatchSequence = min(nextSequence + batchLimitOffset, availableSequence)`, calls `onBatchStart(batchSize, queueDepth)` if the handler implements it, then for each sequence calls `eventHandler.onEvent(event, nextSequence, nextSequence == endOfBatchSequence)`, and only then publishes progress with `sequence.set(endOfBatchSequence)` [batch-proc].
- The paper: a consumer that finds the cursor "has advanced a number of steps since it last checked it can process up to that sequence without getting involved in the concurrency mechanisms", so a lagging consumer regains pace when producers burst; "this effect results in a close to constant time for latency regardless of load, up until the memory sub-system is saturated, and then the profile is linear following Little's Law", unlike "the J curve effect on latency we have observed with queues" [paper-pdf].
- `endOfBatch` lets a handler flush (the paper's `onEndOfBatch` is "for flushing results to an IO device if necessary") [paper-html]; the guide's "Early Release" example reports partial progress with `sequenceCallback.set(sequence)` [guide].
- A `RewindableException` from a `RewindableEventHandler` invokes the `BatchRewindStrategy`; the default `SimpleBatchRewindStrategy` always rewinds to `startOfBatchSequence` and replays the batch. Any other `Throwable` goes to the `ExceptionHandler` and the failing sequence is skipped [batch-proc] [guide].
- Producers may claim and publish ranges: `next(n)` and `publish(lo, hi)`; the multi-producer version marks each slot and signals once [multi-seq].

## 9. How the Disruptor hands results back: multicast and dependency graphs

- Every handler sees every event: the Disruptor "publishes all events to all consumers", unlike a queue, which gives each event to one consumer [guide].
- Dependencies are `SequenceBarrier`s. `ProcessingSequenceBarrier` holds the cursor, a `dependentSequence` (the cursor itself if there are no dependencies, else a `FixedSequenceGroup` of the upstream handlers' sequences), and the sequencer; `waitFor` returns the wait strategy's result and then `sequencer.getHighestPublishedSequence(sequence, availableSequence)` [barrier]. `handleEventsWith(a, b).then(c)` builds the graph; the guide's example runs journal and replication handlers in parallel and the business handler after both [guide].
- Producers are gated by the leaf consumers only: "The sequencer only needs to track the leaf consumers"; `RingBuffer.addGatingConsumers()` registers them [guide]. The paper: "These consumer sequences allow the producers to track consumers to prevent the ring from wrapping" [paper-html].
- The paper's topologies are Unicast 1P-1C, Pipeline 1P-3C, Sequencer 3P-1C, Multicast 1P-3C, and Diamond 1P-3C (two handlers in parallel then a third) [paper-html]. All share "a single ring buffer at the core"; "Care must be taken in the design of such a structure so that the state written by independent consumers does not result in false sharing of cache lines" [paper-pdf].
- Replying to the producer is not described: neither the paper nor the user guide returns anything to the thread that published. The only hand-back described is downstream progress via `Sequence` values (the handler's own sequence, or the `sequenceCallback`) [guide] [paper-html].
- Shutdown: `halt()` sets the state and calls `sequenceBarrier.alert()`, which wakes blocked waiters through `signalAllWhenBlocking`; waiters see `AlertException` and exit [batch-proc] [barrier].

## Sources

- [paper-pdf] Thompson, Farley, Barker, Gee, Stewart, "Disruptor: High performance alternative to bounded queues for exchanging data between concurrent threads", v1.0, May 2011 (PDF metadata 22 June 2011). https://lmax-exchange.github.io/disruptor/files/Disruptor-1.0.pdf
- [paper-html] The same paper rendered as HTML, "Version 1.0 / Last updated 2025-04-02", which adds the AMD EPYC / Disruptor 4 table. https://lmax-exchange.github.io/disruptor/disruptor.html
- [wiki-perf] Disruptor wiki, Performance Results. https://github.com/LMAX-Exchange/disruptor/wiki/Performance-Results
- [guide] Disruptor User Guide. https://lmax-exchange.github.io/disruptor/user-guide/index.html
- [sequence] `Sequence.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/Sequence.java
- [single-seq] `SingleProducerSequencer.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/SingleProducerSequencer.java
- [multi-seq] `MultiProducerSequencer.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/MultiProducerSequencer.java
- [ring-src] `RingBuffer.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/RingBuffer.java
- [batch-proc] `BatchEventProcessor.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/BatchEventProcessor.java
- [barrier] `ProcessingSequenceBarrier.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/ProcessingSequenceBarrier.java
- [blocking] `BlockingWaitStrategy.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/BlockingWaitStrategy.java
- [sleeping] `SleepingWaitStrategy.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/SleepingWaitStrategy.java
- [yielding] `YieldingWaitStrategy.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/YieldingWaitStrategy.java
- [busyspin] `BusySpinWaitStrategy.java`. https://github.com/LMAX-Exchange/disruptor/blob/master/src/main/java/com/lmax/disruptor/BusySpinWaitStrategy.java
- [spsc-readme] rigtorp/SPSCQueue README. https://github.com/rigtorp/SPSCQueue
- [spsc-src] `include/rigtorp/SPSCQueue.h`. https://github.com/rigtorp/SPSCQueue/blob/master/include/rigtorp/SPSCQueue.h
- [mpmc-readme] rigtorp/MPMCQueue README. https://github.com/rigtorp/MPMCQueue
- [mpmc-src] `include/rigtorp/MPMCQueue.h`. https://github.com/rigtorp/MPMCQueue/blob/master/include/rigtorp/MPMCQueue.h
- [folly-doc] folly `docs/ProducerConsumerQueue.md`. https://github.com/facebook/folly/blob/main/folly/docs/ProducerConsumerQueue.md
- [folly-src] folly `ProducerConsumerQueue.h`. https://github.com/facebook/folly/blob/main/folly/ProducerConsumerQueue.h
- [folly-align] folly `lang/Align.h`. https://github.com/facebook/folly/blob/main/folly/lang/Align.h
- [boost-intro] Boost.Lockfree introduction and terminology. https://www.boost.org/doc/libs/release/doc/html/lockfree.html
- [boost-rationale] Boost.Lockfree rationale (memory management, ABA prevention). https://www.boost.org/doc/libs/release/doc/html/lockfree/rationale.html
- [boost-spsc-src] `boost/lockfree/spsc_queue.hpp`. https://github.com/boostorg/lockfree/blob/develop/include/boost/lockfree/spsc_queue.hpp
- [boost-queue-src] `boost/lockfree/queue.hpp`. https://github.com/boostorg/lockfree/blob/develop/include/boost/lockfree/queue.hpp
- [boost-prefix] `boost/lockfree/detail/prefix.hpp` (cacheline_bytes). https://github.com/boostorg/lockfree/blob/develop/include/boost/lockfree/detail/prefix.hpp
- [cpp-order] cppreference, `std::memory_order`. https://en.cppreference.com/w/cpp/atomic/memory_order
- [cpp-atomic] cppreference, `std::atomic`. https://en.cppreference.com/w/cpp/atomic/atomic
- [cpp-wait] cppreference, `std::atomic::wait`. https://en.cppreference.com/w/cpp/atomic/atomic/wait
- [cpp-interference] cppreference, `std::hardware_destructive_interference_size`. https://en.cppreference.com/w/cpp/thread/hardware_destructive_interference_size
- [mappings] Sewell et al., "C/C++11 mappings to processors". https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
- [llvm-atomics] LLVM Atomic Instructions and Concurrency Guide. https://llvm.org/docs/Atomics.html
- [gcc-warn] GCC Warning Options, `-Winterference-size`. https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
- [gcc-params] GCC `params.opt`, `destructive-interference-size`. https://github.com/gcc-mirror/gcc/blob/master/gcc/params.opt
- [gcc12] GCC 12 release notes. https://gcc.gnu.org/gcc-12/changes.html
- [libstdcxx-new] libstdc++ `<new>`. https://github.com/gcc-mirror/gcc/blob/master/libstdc%2B%2B-v3/libsupc%2B%2B/new
- [libcxx] libc++ C++17 status, P0154R1. https://libcxx.llvm.org/Status/Cxx17.html
- [msvc-new] Microsoft Learn, `<new>` typedefs. https://learn.microsoft.com/en-us/cpp/standard-library/new-typedefs?view=msvc-170

Not read: Dmitry Vyukov's bounded MPMC queue page (1024cores.net now redirects to an unrelated host, so it was not followed); Arm's developer documentation on `LDAR`/`STLR` (the page body did not render). The AArch64 mapping facts above come from the Cambridge mappings page only.
