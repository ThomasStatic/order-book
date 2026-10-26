# Profile of the baseline

Where the engine spends its time on the benchmark workloads of [`BENCHMARKS.md`](BENCHMARKS.md), measured on the same code, build, and machine as that baseline, and the three bottleneck hypotheses that follow from it. The raw profiler output is in [`benchmarks/results/2026-10-26-profile/`](../benchmarks/results/2026-10-26-profile/), with the exact commands in its README; the tools are described in [`benchmarks/profiling/README.md`](../benchmarks/profiling/README.md).

| | |
|---|---|
| Date | 2026-10-26 |
| Code | commit `ab59f5c` on `main` (the baseline's code; the profiling tools were added in the working tree) |
| Build | the baseline's Release build (`/O2 /Ob2 /DNDEBUG`, `/OPT:REF /OPT:ICF`) plus symbols (`/Zi`, `/DEBUG`), in `build/profile` |
| Machine | the reference machine of [`benchmarks/README.md`](../benchmarks/README.md#environment) |
| Sampling profiles | `sampling_profiler`, 1 ms interval, extended size (1,000,000 timed commands per run): all five workloads on the core path (20 runs each, 1,700-5,700 samples in the timed regions), and `small-book`, `deep-book`, and `matching-heavy` on the replay path (8 runs each, 3,100-4,500 samples) |
| Allocation counts | `allocation_profile`, standard size (100,000 timed commands), every workload, both paths |

## Method

Two tools, both written for this (there is no usable command-line profiler on the machine).

- **Sampling.** `sampling_profiler` runs the unmodified benchmark program and, once a millisecond, suspends its main thread, copies its registers and stack, resumes it, and walks the copy with dbghelp. The walk includes inline frames, so `PriceLevel::addOrder`, `Order::getRemainingQuantity`, and the standard library's internals appear as their own frames although the compiler inlined them. A sample's leaf frame is where the thread was executing (self time); every frame on its stack gets inclusive time. Only samples inside the runner's timed regions (`runCore`, `runReplay`) are counted, with the untimed work the runner does around them (generating the workload, the preload, rendering the CSV, the outcome check, destroying the book) dropped. The shares below are of those samples. The benchmark's throughput under the profiler was 5-27% below the unprofiled baseline (the large-book workloads most), since the profiler takes a core and competes for the memory system; samples are taken on a clock independent of the target, so the shares are representative.
- **Counting.** `allocation_profile` replaces global `operator new`/`delete`, runs each workload once, and charges every allocation to the command that made it, with a histogram of sizes. Node sizes identify the container: 48 bytes is a `std::list<Order>` node (16 bytes of links plus the 32-byte `Order`), 40 bytes an order-index node (`std::unordered_map` keeps its nodes in a list: links plus the key and the 16-byte `OrderLocation`), 72 bytes a price-level map node (`std::map` node header plus the 32-byte `PriceLevel`), 20 bytes a result's `std::vector<BookEvent>` holding one event, 96 bytes the trades vector that `matchOrder` reserves for four trades. For the replay path it also times the three stages the benchmark's `replay` kind covers: parsing, running the records, formatting the summary.

Limits: one profiled run per workload and kind, so a share below about 1% is noise; `ntdll` has no symbols here, so its heap internals resolve to the nearest exported name (`RtlAllocateHeap`, `RtlFreeHeap`, and for one internal function `RtlUpcaseUnicodeString`); the linker's identical-code folding labels the destructor of the results' event and trade vectors as `std::vector<BenchmarkCommand>::~vector`, which has the same code; time in the kernel (page faults) is charged to the user instruction that caused it. Nothing here measures a single command's latency.

## Results

### Core path

Shares of the samples in the timed region, per workload. The first block is self time (the thread was executing in that function); the second is inclusive time (the function was on the stack). The container rows are inclusive time in any function of that container.

| | small-book | matching-heavy | deep-book | cancellation-heavy | mostly-resting |
|---|---:|---:|---:|---:|---:|
| samples | 1,739 | 2,897 | 5,690 | 3,900 | 5,148 |
| **self: heap allocate** (`RtlAllocateHeap` and below, `malloc`, `operator new`) | 31.4 | 27.1 | 20.7 | 22.6 | 28.3 |
| **self: heap free** | 15.7 | 13.2 | 13.5 | 16.9 | 9.3 |
| **self: order-index lookup** (`unordered_map::_Find_last`) | 4.1 | 10.3 | 25.0 | 19.8 | 22.4 |
| **self: order-index rehash** (`_Forced_rehash`) | 0.0 | 0.0 | 0.0 | 0.0 | 11.1 |
| **self: price-level lookup** (`map::_Find_lower_bound`, both sides) | 4.9 | 4.0 | 11.7 | 10.1 | 5.1 |
| **self: `Order::getRemainingQuantity`** (the first touch of a resting order's node in `cancelOrder`) | 1.3 | 1.9 | 4.9 | 6.9 | 1.0 |
| inclusive: `submitOrder` | 66.2 | 79.7 | 58.3 | 43.9 | 82.9 |
| inclusive: `cancelOrder` | 19.5 | 7.0 | 33.4 | 46.5 | 6.3 |
| inclusive: `validateNewOrder` (the duplicate-ID lookup) | 5.2 | 10.4 | 14.4 | 10.0 | 22.2 |
| inclusive: `restOrder` (level `try_emplace`, list insert, index emplace) | 24.9 | 20.3 | 23.9 | 25.5 | 46.9 |
| inclusive: `matchOrder` | 15.1 | 27.1 | 9.1 | 0.0 | 2.6 |
| inclusive: `SubmissionResults::accepted` | 19.1 | 19.2 | 8.2 | 6.2 | 9.3 |
| inclusive: `cancelOrder`'s removal step (level find, list erase, index erase) | 9.7 | 3.7 | 15.5 | 21.7 | 3.0 |
| inclusive: `std::unordered_map` (the order index) | 19.3 | 22.0 | 40.3 | 40.4 | 49.9 |
| inclusive: `std::map` (the price levels) | 29.1 | 23.6 | 29.9 | 30.5 | 47.1 |
| inclusive: `std::list` (level queues and the index's node list) | 23.5 | 21.3 | 23.6 | 29.0 | 27.9 |
| inclusive: `std::vector` (results' events and trades) | 34.0 | 31.7 | 16.2 | 15.2 | 17.6 |

The container rows overlap (an index insertion allocates a list node inside the hash table, and so on), and `mostly-resting`'s `std::map` share is inflated by the rehash, which runs inside `restOrder`.

Exact allocation counts at the standard size, per timed command, from `allocations.txt`:

| Command kind | Allocations | Frees | Bytes | What they are |
|---|---:|---:|---:|---|
| submit that rests without trading | 3.0 | 1.0 | 108 | a list node (48), an index node (40), the result's one-event vector (20); plus a map node (72) and a list sentinel node (48) when the price level is new |
| submit that trades then rests the remainder | 6.0-6.1 | 6.4-8.4 | 348-381 | the trades vector (96, reserved for four), the events vector, a new level (72 + 48: the remainder rests at a price the opposing side just vacated), the list and index nodes; the frees are the filled orders' list and index nodes and the emptied levels |
| submit filled completely | 2.0-2.1 | 2.7-4.4 | 150-204 | the trades vector (96) and the events vector (40 bytes for one trade, 20 more per further trade; a sweep of more than four trades reallocates the trades vector: 192, 384, 768) |
| cancel | 1.0 | 3.0 | 20 | the result's one-event vector; frees the list node, the index node, and the level when emptied |

The order index rehashed 0 times during the timed commands of the four steady-state workloads (their tables were sized by the preload) and 7 times in `mostly-resting` at the standard size (1,024 buckets to 131,072), each rehash walking every node; at the extended size it grows to about a million buckets, and the walks are the 11% above. The index's load factor sits at 0.6-0.76 against a maximum of 1.0, so the bucket array has 131,072 entries of 16 bytes (2 MiB) for `deep-book`'s 100,000 orders.

### Replay path

| | small-book | matching-heavy | deep-book |
|---|---:|---:|---:|
| samples | 3,092 | 3,716 | 4,544 |
| self: heap allocate | 22.7 | 22.7 | 19.8 |
| self: heap free | 12.3 | 11.0 | 12.6 |
| self: `streambuf::snextc` (one virtual call per character inside `std::getline`) | 10.6 | 10.1 | 7.3 |
| inclusive: `readReplayInput` (parsing) | 76.2 | 67.0 | 52.8 |
| inclusive: `ReplayReader::readLine` (`std::getline`) | 26.8 | 25.5 | 19.4 |
| inclusive: `splitRecord` | 30.4 | 24.7 | 20.5 |
| inclusive: `parseRow` | 10.4 | 9.6 | 7.4 |
| inclusive: `ReplayEngine::run` (dispatch, the book's commands, event collection) | 23.5 | 32.8 | 47.0 |
| inclusive: `submitOrder` + `cancelOrder` within it | 18.0 | 23.2 | 41.5 |
| inclusive: `collectEvents` | 3.5 | 4.7 | 3.5 |
| inclusive: strings and streams | 63.9 | 55.9 | 44.2 |

`allocation_profile` counts exactly 6 allocations and 6 frees per record in parsing, 710 bytes: `splitRecord` grows its `std::vector<std::string>` through capacities 1, 2, 3, 4, and 6 (32, 64, 96, 128, and 192 bytes) for six fields, and `std::getline` allocates the line's buffer (32 bytes; a record is 26-29 characters, past the small-string limit). The six field strings themselves fit the small-string buffer. Its one-run stage timings at the standard size: parsing 610-800 ns per command, running 260-550 (which includes the book's own 160-450), formatting the summary under 5. The baseline's 580-650 ns difference between the replay and core paths is parsing.

## Expensive functions

In order of self time across the core profiles:

1. **The heap allocator**, `RtlAllocateHeap` and `RtlFreeHeap` with the CRT's `malloc`/`free` and `operator new`/`delete` wrappers: 34-47% of the core path's samples in every workload, and 32-35% of the replay path's. It is the largest single cost everywhere, and the only large one in `small-book` and `matching-heavy`, whose books fit in cache.
2. **`std::unordered_map::_Find_last`**, the order index's bucket walk: 20-25% self in the three large-book workloads (`deep-book`, `cancellation-heavy`, `mostly-resting`) and 4% in `small-book`. The hot lines (`xhash:1588-1596`) load the bucket's entry and then compare the key in the node, two dependent loads into a 2 MiB bucket array and nodes scattered over the heap; the share rises with the book, which is the signature of cache misses, not of instructions. Every submit does this lookup once (`validateNewOrder`'s duplicate check), every cancel does it once to find the order and then again inside `erase(iterator)`, which rehashes the key to find the bucket's bounds.
3. **`std::map::_Find_lower_bound`**, the price-level lookup: 10-12% self in `deep-book` (2,000 levels, about 11 node hops) and `cancellation-heavy` (500 levels), 4-5% elsewhere. Every resting submit walks it in `try_emplace`, every cancel in `find`.
4. **`std::unordered_map::_Forced_rehash`**: 11% of `mostly-resting`, the only workload whose book outgrows its preload's table.
5. **`Order::getRemainingQuantity`** at 5-7% in the cancel-heavy workloads: it is one load, charged the miss on the resting order's list node, the first time `cancelOrder` touches it.
6. On the replay path, **`std::getline` through `streambuf::snextc`** (7-11% self; the stream fetches one character per virtual call) and **`splitRecord`** (20-30% inclusive, nearly all of it allocation).

Smaller items: `SubmissionResults`' private constructor assigns `generatedTrades = {}`, which runs the vector's range-assign (`_Assign_counted_range`, about 2% in `small-book`) before the trades are moved in; `Order::malformedReason` runs twice per submit (constructor and `validateNewOrder`), about 1%; the runner's `execute` itself is 3-5% (result destruction, tallying).

## Allocation-heavy paths

- **Every resting submission makes three allocations and every cancellation one**, and each command frees what the other allocated: the `std::list<Order>` node (`PriceLevel::addOrder`), the index node (`orderIndex.emplace` in `restOrder`), and the result's `std::vector<BookEvent>`, which goes to the heap for a single 20-byte event (`SubmissionResults::accepted` and `CancellationResult`'s constructor). In the sampling profiles these three sites plus their frees are the whole heap share of the resting workloads: `PriceLevel::addOrder` 6-13%, `restOrder`'s index emplace 7-10%, the events vector 3-5% to allocate and 4-7% to free (the `~vector<BenchmarkCommand>` rows).
- **Matching adds the trades vector**, reserved for four trades (`matchOrder`, 96 bytes, 4-5% of `matching-heavy`) and grown by doubling on longer sweeps, and the events vector then holds one event per trade. `SubmissionResults::accepted` is 19% inclusive of `matching-heavy` and `small-book`, most of it this allocation.
- **A new price level costs two allocations** (map node and list sentinel) and an emptied one two frees; a submit that trades and then rests always creates one, so it makes about six allocations and frees six to eight.
- **Parsing a replay record makes six allocations**: five reallocations of `splitRecord`'s field vector and the line buffer in `readLine`. The `ReplayRecord` and `ReplayEvent` vectors grow amortized and are not significant.

## Map and hash-map activity

- **Order index** (`std::unordered_map<OrderId, OrderLocation>`): inclusive 19-22% of the cached workloads and 40-50% of the large-book ones. Per submit: one `find` (duplicate check) and one `emplace` (which allocates a node, links it into the table's node list, and checks for rehash). Per cancel: one `find`, one `erase(iterator)`, which recomputes the hash (`_Fnv1a_append_value`, since the table does not cache it) to fix the bucket's first/last pointers, then frees the node. Per trade that fills a resting order: one `erase(key)`. The table never rehashes in steady state; `mostly-resting` rehashes as it grows (7 times at the standard size), walking every node each time. `std::hash<unsigned int>` is FNV-1a over the four bytes (`_Fnv1a_append_bytes`, about 2% in `small-book`): cheap, but not free, and it spreads sequential IDs over the whole 2 MiB bucket array, so consecutive orders never share a cache line in the index.
- **Price levels** (`std::map<Price, PriceLevel>` per side): inclusive 24-31% (47% in `mostly-resting`, where it contains the rehash). Per resting submit: one `try_emplace` (`_Find_lower_bound` then, for a new price, a node allocation and rebalancing). Per cancel: one `find`, and an `erase` when the level empties. Matching reads `begin()` (constant time in this implementation) and erases the front level when it empties. The cost is the walk: 11 dependent loads at 2,000 levels, each a likely miss once the book is large.
- **Level queues** (`std::list<Order>`): `insert` at the back and `erase`/`pop_front`, each one node allocation or free; `_Transfer_before` (relinking) is 1-2% in `deep-book`, the miss on the level's tail node.

## The three strongest bottleneck hypotheses

Ordered by the share of time they explain and how much of it a fix could recover. Each gives the evidence, the prediction a fix would have to meet, and how to check it before building anything.

### 1. Per-command heap allocation is the largest cost on every workload

**Claim.** Three allocations per resting submit, one per cancel, two to six per matching submit, and one free for each, account for 34-47% of the core path. Most of them exist only to hand the result back: the events vector allocates for a single event on every command, and the trades vector reserves four `Trade`s for every match. The engine's own data structures add the list node and the index node per order, which a pool could serve without touching the general-purpose heap.

**Evidence.** The heap's self time is the top row of every core profile; `allocation_profile` fixes the count at 3.0 / 1.0 / 2.0-6.1 allocations per command with the sizes above; the three allocation sites and their frees are the top heap rows in every report (`PriceLevel::addOrder` through `list::insert`, `restOrder` through `unordered_map::emplace`, `SubmissionResults::accepted` and `CancellationResult` through `vector::reserve`/construction, and their destructors in `execute`). `small-book`, whose working set fits in cache and whose book barely changes shape, spends 47% of its 165 ns per command there.

**Prediction.** Removing the events and trades vectors' heap use (an inline buffer for the common one-to-four events, or returning them in a caller-supplied buffer) should cut `small-book` and `matching-heavy` by 15-25% of their time; pooling list and index nodes should take most of the rest of the heap share, bringing the recoverable total to 30-40% on the cached workloads and 20-30% on the large-book ones (where the misses below remain).

**Check.** `allocation_profile` shows the count falling; the sampling report's heap rows and `~vector<BenchmarkCommand>` rows shrink by the same share; `small-book`'s ns/command falls by the predicted amount with its CV unchanged.

### 2. The order index misses cache on every lookup once the book is large, and its growth rehashes

**Claim.** `std::unordered_map` with FNV-1a hashing scatters the index over a 2 MiB bucket array (16 bytes per bucket, load factor under 1) and 40-byte nodes allocated one by one; each lookup is two dependent loads into memory that the previous command did not touch, and each command does one to three lookups. In `deep-book`, `cancellation-heavy`, and `mostly-resting` that is 20-25% self time in the bucket walk alone and 40-50% inclusive in the index, against 4% and 19% in `small-book`, whose index fits in L2. The level map's walk (10-12%) and the first touch of the cancelled order's node (5-7%) are the same effect in the other two structures. The run-to-run noise the baseline recorded for exactly these three workloads (CV 11-22%, session medians 23-48% apart) and `mostly-resting`'s rise from 300 to 440 ns per command as its book grows tenfold are what a memory-bound loop looks like; a compute-bound one would be stable.

**Evidence.** The self-time rows for `_Find_last` (`xhash:1588-1596`: the bucket load and the key compare in the node), `_Find_lower_bound` (`xtree:1632`: the key compare in the tree node), and `getRemainingQuantity` (`order.cpp:68`: the first load from the list node) rise together with book size; `_Forced_rehash` is 11% of `mostly-resting` and absent elsewhere; `cancelOrder`'s inclusive share in `cancellation-heavy` is 46%, of which the index's `find` + `erase` is 22%.

**Prediction.** An index with locality (a flat open-addressed table keyed by ID, or a dense array when IDs are allocated sequentially, with a cheaper or identity hash) should remove most of the 20-25% self time in `_Find_last` and the 2-4% in `erase`'s rehash, and sizing it ahead removes the 11% rehash in `mostly-resting`. Placing orders in a pool (contiguous, reused slots) would also take the 5-7% first-touch miss in cancellation. Expected: 25-35% off `deep-book` and `cancellation-heavy`, more off `mostly-resting`, and a lower CV. `small-book` would barely move, which is the control.

**Check.** Before changing the structure, confirm the miss hypothesis cheaply: run `deep-book` with the index's `reserve` set from the preload (no change expected: it is already sized) and with `max_load_factor` lowered and raised (a larger table should be slower per lookup if it is misses, not instructions), and compare `deep-book` at the smoke, standard, and extended sizes for the same ns/command (steady-state books should be flat if the cost is in the structure and not in growth). Hardware counters would settle it directly, but no counter tool is available on this machine.

### 3. The replay path's cost is CSV tokenization that allocates per record, not the engine

**Claim.** Parsing is 53-76% of the replay path and 610-800 ns per command on its own, two to four times the engine's cost on the same commands, and nearly all of it is allocation (six per record, in `splitRecord`'s growing `std::vector<std::string>` and `getline`'s line buffer) plus `getline` pulling characters one at a time through `streambuf`'s virtual interface. The engine, event collection (3-5%), the final snapshot, and the summary are not where the replay time goes.

**Evidence.** `readReplayInput` 53-76% inclusive, `splitRecord` 20-30%, `readLine`/`getline` 19-27%, `snextc` 7-11% self, `parseRow` (the actual field validation and number conversion) only 7-10%; the top two heap rows of every replay profile are `splitRecord`'s `emplace_back` allocating and freeing; `allocation_profile` counts exactly six allocations and 710 bytes per record in parsing and times parsing at 610-800 ns per command against 260-550 for running.

**Prediction.** A tokenizer that splits the line in place (six `std::string_view`s into one reused line buffer, or into the in-memory text itself) and reads lines without `getline`'s per-character path would remove all six allocations and most of the stream time, cutting parsing from about 600-800 ns to well under 200 per command and the replay path's total by roughly 40-50%. The format's checking order and diagnostics (`docs/replay-format.md`) are unaffected by how fields are tokenized; `CsvRow`'s public shape is, so this is a design change to the parser's interface, not a local fix.

**Check.** `allocation_profile`'s parse stage shows zero allocations per record and the new time; `replay_parser_tests` and `replay_engine_tests` still pass byte-for-byte on every sample; the replay profile's `readReplayInput` share falls toward the engine's.

### Not among the three

The price-level map's walk (10-12% in deep books) is real but smaller and bound up with hypothesis 2's locality question (a flat or array-backed level store would address both); the duplicate `malformedReason` check and the no-op trades assignment in `SubmissionResults`' constructor are each about 1-2% and worth fixing only in passing.
