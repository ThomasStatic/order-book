# Optimization experiments

Experiments designed from the profile of the baseline ([`PROFILING.md`](PROFILING.md)), one per candidate change. None of them has been run. Each states the bottleneck the profile observed, the hypothesis the change tests, which workloads should move and which should not (the controls), the correctness risks, the readability cost, and the measurement that decides it. The engine's contracts in `CLAUDE.md` (results built before mutation, `cancelOrder` throwing nothing once its lookup passes, no resting event, byte-exact replay output) are the correctness bar for every one of them.

Starting points, from the profile:

| Observed | Share of the core path's time |
|---|---|
| heap allocation and free (`RtlAllocateHeap`, `RtlFreeHeap` and wrappers) | 34-47% on every workload; 3 allocations per resting submit, 1 per cancel, 2-6 per matching submit |
| order-index bucket walk (`unordered_map::_Find_last`) | 20-25% on the large-book workloads, 4% on `small-book` |
| order-index rehash while the book grows (`_Forced_rehash`) | 11% of `mostly-resting`, 0 elsewhere |
| price-level map walk (`map::_Find_lower_bound`) | 10-12% on `deep-book` and `cancellation-heavy`, 4-5% elsewhere |
| first touch of the cancelled order's node (`getRemainingQuantity`) | 5-7% on the cancel-heavy workloads |
| replay parsing (`readReplayInput`) | 53-76% of the replay path; 6 allocations per record |

## How every experiment is measured

The same protocol for all of them, so results are comparable with [`BENCHMARKS.md`](BENCHMARKS.md) and with each other.

- **Build.** The baseline's flags (Visual Studio generator, Release, `/O2 /Ob2 /DNDEBUG`, no LTO, no `/arch`), `ORDER_BOOK_BUILD_BENCHMARKS=ON`. Keep the unchanged code's `order_book_benchmarks.exe` beside the experiment's (two build directories) so both run in the same session.
- **Runs.** `benchmarks/compare_ab.py`, at the standard size with both kinds and seed 42. For each workload it makes 16 pairs of short invocations of the two binaries (each 1 warm-up and 5 measured runs), in ABBA order, so each binary runs first and second equally often and the two halves of a pair see nearly the same machine state.
- **Comparison.** Each pair gives one ratio, the experiment's median throughput over the base's. The result per workload and kind is the median of the 16 ratios, a bootstrap 95% confidence interval for it, and the share of pairs in which the experiment was faster. Each experiment below names the size of change it predicts. A predicted gain that does not appear at that size is a failed experiment, and the change is reverted and the result written down here, not kept because it "should" help.
- **Resolution.** A base-against-base run (the same binary copied to a second file) is the noise floor. Its median ratios lie between 0.94 and 1.00, all ten of them below 1, with intervals that include 1 on nine rows (`benchmarks/results/2026-10-26-experiments/null-paired.md`). A consistent 1-2% offset between two copies of one binary is most likely the physical placement of their code pages, which the protocol cannot remove. So a difference within about ±3% is treated as no measurable change, and `mostly-resting` `core`, whose interval spans 0.89-1.02 in that run, needs a larger margin still.
- **Why not whole sessions.** The protocol first planned here ran three sessions of 30 runs per binary, alternating whole sessions. Its base-against-base run measured the identical binary 9% faster on `matching-heavy` in every one of the three sessions, only because it ran second, and per-session ratios on the large-book workloads ranged from 0.62 to 1.76 (`null-sessions.md`). It could not resolve a change under 10%, and was replaced before any experiment was judged by it.
- **Mechanism.** Throughput alone cannot say whether a change did what it was meant to, so each experiment also names a deterministic or sampled check: `allocation_profile`'s per-command counts and size histogram, or a row of the sampling report (`benchmarks/profiling/README.md`). A change whose mechanism check passes but whose throughput does not move is a finding too: that cost was not on the critical path.
- **Correctness gate.** The whole test suite in Debug and Release (`allocation_failure_tests` only runs in Release; `invariant_tests`, `scenario_tests`, `randomized_regressions`, every `replay_cli_*` case byte for byte), the benchmark's own outcome check (the harness throws when trades, volume, or the final book differ from the prediction), and the workload fingerprints unchanged, since no experiment touches the generator.

## Summary

| # | Experiment | Bottleneck | Should improve | Predicted gain | Risk | Readability cost |
|---|---|---|---|---|---|---|
| 1 | Remove unnecessary copies and no-op work | small fixed costs per command | `small-book`, `matching-heavy` | 2-4% | very low | none; the code gets shorter |
| 2 | Size the order index ahead; sweep the load factor | rehash on growth; bucket-array size | `mostly-resting` (a); diagnostic (b) | 5-10% on `mostly-resting` at the standard size, up to 11% at the extended size | low | low (one hint on the book) |
| 3 | Reduce repeated lookups (ceiling measurement first) | second index walk per submit and per cancel | `deep-book`, `cancellation-heavy` | 0-5%; the ceiling is measured before anything is built | low to medium | low to medium |
| 4 | Inline storage for results' events and trades | per-command result allocations | `small-book`, `matching-heavy`, then all | 15-25% on the cached workloads, 8-12% on the large-book ones | medium (API change, exception contracts) | medium (a small-buffer type) |
| 5 | Level handle in `OrderLocation` | map walk on every cancel and level erase | `cancellation-heavy`, `deep-book` | 3-6% | medium (dangling handle, invariant checker) | medium (comparator unification, bigger location) |
| 6 | Node pools for level queues and the index | the remaining allocation share, and node locality | all; most on `cancellation-heavy`, `deep-book` | 15-25% beyond experiment 4 | medium to high (allocation-failure semantics, moves) | high (allocator type in every header) |
| 7 | Aggregate-quantity maintenance | none observed | none predicted | 0% (a null experiment) | low | none |
| 8 | Replace the index container (and later the level map) | cache misses in the index on large books | `deep-book`, `cancellation-heavy`, `mostly-resting` | 25-35% on those | high (hand-written table, test access) | high |
| 9 | Replay tokenizer without allocation | CSV parsing cost | every `replay` kind | 40-50% of the replay path | medium (parser interface) | medium |

Order: 1 and the diagnostic halves of 2 and 3 first, since they are cheap and sharpen the predictions of the rest; then 4 and 5; 6 and 8 only once 2(b) and 6 have confirmed that the large-book cost is misses rather than instructions; 7 at any time as a check on the method; 9 independently, since it is the replay path alone.

## 1. Remove unnecessary copies and no-op work

**Observed bottleneck.** Small costs that the profile charges to code that does nothing useful: `SubmissionResults`' private constructor assigns `generatedTrades = {}` before the trades are moved in, which runs the vector's range-assign (`_Assign_counted_range`, about 2% of `small-book`'s self time); `restOrder` stores the location in a `std::optional<OrderLocation>` for its rollback path (`_Optional_construct_base::_Assign`, 1-1.5%); `snapshot` copies its two summary vectors into the result instead of moving them (once per replay run, at full depth); `addOrder` and `PriceLevel::addOrder` take `Order` by value and the level copies it again into the list node (`Order` is 32 trivially copyable bytes, so this is cheap, and only listed for completeness).

**Hypothesis.** These are pure overhead; removing them lowers the fixed cost per command by a few percent without changing any data structure.

**Should improve.** `small-book` and `matching-heavy` `core`, whose cost per command is mostly fixed overhead, by 2-4%. The large-book workloads are controls: their time is in misses and allocation, and should not move measurably.

**Correctness risks.** Very low. The constructor change keeps `generatedTrades` default-constructed (empty), which is what the assignment produced. Replacing the `optional` with a flag or a restructured rollback must keep the rollback exact: a failed index emplace removes the just-inserted order and erases a level it created (`submission_tests` and `allocation_failure_tests` cover both). Moving the snapshot's vectors changes nothing observable.

**Readability costs.** None; each change deletes code or replaces a copy with a move.

**Measurement.** The sampling report's `_Assign_counted_range` and `_Optional_construct_base::_Assign` rows disappear from `small-book-core`; `small-book` `core` median throughput rises by at least 2%, which is above its 1% session spread; `allocation_profile`'s counts are unchanged (this experiment removes no allocation).

## 2. Size the order index ahead, and sweep its load factor

**Observed bottleneck.** `std::unordered_map::_Forced_rehash` is 11% of `mostly-resting` at the extended size (the index grows from 1,024 to about a million buckets, walking every node at each doubling) and 7 rehashes at the standard size; it is 0% of the steady-state workloads, whose preload sized the table. Separately, the bucket array is 16 bytes per bucket at a load factor of 0.6-0.76 (2 MiB for `deep-book`), and the profile's cache-miss reading of `_Find_last` (hypothesis 2 of the profile) has not been tested directly.

**Hypothesis.** (a) A capacity hint from the caller removes rehashing from a growing book; the engine cannot know the final size, so this is an API addition (`OrderBook::reserve(n)` or a constructor argument), used by the benchmark runner in the untimed preload. (b) Diagnostic: if the index's cost is cache misses, a larger bucket array (lower `max_load_factor`) makes lookups slower per command and a smaller one (higher) faster until chains lengthen; if its cost is instructions, neither moves it. This costs one line per variant and decides whether experiments 6 and 8 are worth building.

**Should improve.** (a) `mostly-resting` only, by about 5-10% at the standard size and up to 11% at the extended size; every other workload is a control and must not change (their tables were already sized). (b) is not meant to improve anything: it predicts a slope. `deep-book` and `cancellation-heavy` should get slower with `max_load_factor` 0.25 and faster, or no slower, with 2.0; `small-book` should not move at any setting.

**Correctness risks.** (a) None for the book's semantics; `reserve` changes only when rehashes happen. The benchmark harness must apply the hint in the preload, outside the timed region, and record it (a workload run with and without the hint are different environments). Memory grows ahead of use. (b) Throwaway; never merged.

**Readability costs.** (a) One public method with a comment on what it does and does not promise. The benchmark README gains a line. (b) None.

**Measurement.** (a) `allocation_profile` reports 0 rehashes during `mostly-resting`'s timed commands at the standard and extended sizes; the sampling report's `_Forced_rehash` row is gone; `mostly-resting` `core` throughput rises by the predicted amount with the other four workloads inside their base-against-base spread. (b) The sign of the change in `deep-book` and `cancellation-heavy` ns/command across `max_load_factor` 0.25, 0.5, 1.0 (the default), 2.0, taken in one interleaved session; a monotone slope confirms the miss hypothesis and is recorded here as the justification for 6 and 8.

## 3. Reduce repeated lookups

**Observed bottleneck.** Each resting submit walks the index twice for the same key: `validateNewOrder`'s duplicate check (`find`, 10-22% inclusive) and then `restOrder`'s `emplace` (7-11% inclusive, which includes the node allocation). Each cancel walks it twice too: `find`, then `erase(iterator)`, which recomputes the hash (`_Fnv1a_append_value` appears under it) and walks the bucket again to fix its bounds (about 10% inclusive of `cancellation-heavy`). Each cancel also walks the level map to find its level (`_Tree::find`, about 4% of `deep-book` and `cancellation-heavy`), which experiment 5 addresses.

**Hypothesis.** The second walk of the same bucket is cache-warm, so its cost is mostly instructions, not misses, and is smaller than its inclusive share suggests. The honest version of this experiment measures the ceiling first: a throwaway build that skips the duplicate check (safe for the generated workloads, which never reuse an ID) bounds what any single-lookup design could gain. Only if the ceiling is worth it is a real design tried: a provisional index entry inserted during validation and completed after resting (which must be removed on a throw and when the incoming order fills completely, adding work to `matching-heavy`), or, more simply, folding the duplicate check into experiment 8's table, whose API can find-or-insert in one probe. The cancel's second walk cannot be removed while the index is `std::unordered_map` (its `erase` always rehashes), so it is deferred to experiment 8.

**Should improve.** `deep-book` and `cancellation-heavy`, where the index is 40% of the time; the ceiling run says by how much. Prediction for the ceiling: 2-5%. `small-book` is the control (its index is in cache; the walk is cheap either way).

**Correctness risks.** The ceiling run is not correct code and is never merged. A provisional-entry design changes the order of mutations the contracts rely on: validation would mutate the index, so a rejection would no longer leave the book untouched unless the entry is removed on every exit, and the invariant checker would see an entry for an order not yet resting if a boundary check ran in between (it does not, but the checker's "one entry per resting order" rule would need a documented exception or the entry must be added after matching). The randomized harness's model and `submission_tests`' before/after `BookState` comparisons are the tests that catch a leaked entry.

**Readability costs.** The ceiling run has none. A provisional-entry design costs real clarity: the submit path's three-step structure (validate, match, rest) is the reason the contracts are easy to state, and an entry that exists before the order rests breaks the one-sentence description of the index. If the ceiling is under 3%, record that and stop.

**Measurement.** The ceiling: `deep-book` and `cancellation-heavy` `core` throughput with and without the duplicate check, interleaved; the sampling report's `validateNewOrder` inclusive row goes to zero in the throwaway build, which confirms the measurement measured what it meant to. A real design is then held to the ceiling: it must recover most of it, pass the full gate, and leave `submission_tests`' rejection comparisons intact.

## 4. Inline storage for results' events and trades

**Observed bottleneck.** Every command allocates to report its result: `SubmissionResults::accepted` reserves a `std::vector<BookEvent>` of one event per resting submit (20 bytes) and `CancellationResult` one of one event; `matchOrder` reserves a `std::vector<Trade>` for four trades (96 bytes) on every match. With their frees these are 12-20% of the cached workloads' samples (`SubmissionResults::accepted` is 19% inclusive of `small-book` and `matching-heavy`, nearly all of it this allocation; the `~vector<BenchmarkCommand>` rows, which are these vectors' destructors under identical-code folding, are 4-7%). `allocation_profile` shows them as exactly 1 allocation per resting submit and cancel and 2 per filled submit.

**Hypothesis.** Events and trades per command are almost always few (one event for a resting submit or cancel; 1.2-2.1 trades per matching submit at the standard size, with sweeps past four rare). A small-buffer container holding up to N elements inline (N = 4, say) and spilling to the heap beyond that removes these allocations from the common path without changing what a result holds.

**Should improve.** `small-book` and `matching-heavy` `core` by 15-25% (their heap share from the results' vectors); the large-book workloads by 8-12% (the same allocations, a smaller share of a longer command). The `replay` kinds improve by the same absolute amount, a smaller share. The control is the order of events and trades, which must not change at all.

**Correctness risks.** Medium. The public accessors return `const std::vector<BookEvent>&` and `const std::vector<Trade>&`; the replay engine's `collectEvents` and about a hundred test sites bind or compare against `std::vector` (`event_tests` compares `getEvents()` to a `std::vector<BookEvent>` literal, `matching_tests`' `expectTrades` takes `const std::vector<Trade>&`). Returning `std::span<const T>` (C++20) keeps range-for, `size`, and indexing working but breaks the comparisons and the reference bindings, so the tests change too; comparing a span to a vector literal needs a helper. The exception contracts must be re-examined: `accepted` validates the trades before building the result, and `cancelOrder` builds its result "before anything is mutated, since recording the event allocates", which a non-allocating result makes trivially true; the spill path past N elements still allocates and must keep the "make room before `executeOnce`" rule in `matchOrder`, which currently relies on `vector::reserve`. `allocation_failure_tests` must still see the book sound at every failure point; with fewer allocations there are fewer points, which is fine, but a small-buffer type with a hand-written spill is new code with its own bugs (copy, move, destruction of the inline elements), and needs its own unit tests like `result_type_tests`.

**Readability costs.** Medium. A `SmallVector<T, N>` (or two fixed-capacity types if spilling is avoided by making the trades vector the only heap case) is a new utility that every reader of the result types meets, and `std::span` in the API is one more type to know. Against that, `cancelOrder`'s comment about allocating before mutation can be simplified.

**Measurement.** `allocation_profile`: a resting submit goes from 3.0 to 2.0 allocations, a cancel from 1.0 to 0.0, a filled submit from 2.0 to 0.0 for up to four trades; the sampling report's `SubmissionResults::accepted` heap row and the `~vector<BenchmarkCommand>` free rows vanish; `small-book` and `matching-heavy` `core` medians rise by at least 10%, well above their spread; `event_tests`, `result_type_tests`, `scenario_tests`, and every replay expected file pass unchanged in content.

## 5. A level handle in `OrderLocation`

**Observed bottleneck.** `cancelOrder` looks the order up in the index, then walks the level map for its price (`levels.find(location.tickPrice)`: `_Tree::find` under the cancel lambda, about 4% of `deep-book` and `cancellation-heavy`, 11 dependent loads at 2,000 levels), then erases the level by that iterator when it empties. `restOrder` had the level's iterator in hand when it indexed the order and threw it away, keeping only the price.

**Hypothesis.** Storing a handle to the level in `OrderLocation` (the map iterator, which is stable for a node-based `std::map` until that node is erased) lets `cancelOrder` go from the index entry straight to the level and erase it without a second walk. The price and side stay in the entry, since the invariant checker and the tests use them.

**Should improve.** `cancellation-heavy` and `deep-book` `core` by 3-6% (the map's `find` share plus the erase's lookup); `matching-heavy` and `mostly-resting` are controls (few cancels).

**Correctness risks.** Medium. A handle can dangle if a level is erased while an order still references it; the invariants forbid that (a level is erased only when empty, after every order in it is unindexed), so the risk is a bug in a new path rather than a design flaw, and `checkInvariants` should verify the handle: compare it with `levels.find(price)` only after the entry has named the right side and price, as it already does for the list iterator, so a stale entry is still never dereferenced. MSVC's checked iterators (Debug) assert on comparing an iterator into an erased node, so the checker's existing discipline matters. `bids` and `asks` have different comparator types, so their iterators differ in type; the handle needs either a `std::variant`, a raw `PriceLevel*` plus erase by key (which brings back a walk on the erase path only), or one comparator type for both sides (a comparator holding the side), which changes the two map declarations. `invariant_tests` build `OrderLocation` by hand to plant wrong entries and must set the new field; `OrderLocation`'s constructor changes.

**Readability costs.** Medium. `OrderLocation` grows from 16 to 24 or more bytes and gains a field whose validity rule has to be stated; a side-aware comparator makes the two maps' types the same at the cost of a less obvious declaration than `std::greater`/`std::less`. The index node grows too, which slightly works against experiment 8's locality.

**Measurement.** The sampling report's `_Tree::find | cancelOrder` rows go to zero and `_Find_lower_bound` self time falls by the cancel share; `cancellation-heavy` and `deep-book` `core` rise by at least 3% in the interleaved sessions; `cancellation_tests`' full before/after `BookState` comparisons, `invariant_tests`' index cases (extended to the new field), and the randomized regressions pass.

## 6. Node pools for the level queues and the index

**Observed bottleneck.** After experiment 4, the remaining allocations per command are the engine's own nodes: one `std::list<Order>` node (48 bytes) and one index node (40 bytes) per resting order, allocated on rest and freed on fill or cancel, plus a map node and a list sentinel per new level. They are 15-25% of each profile (`PriceLevel::addOrder` through `list::insert` 6-13% to allocate; `restOrder`'s index `emplace` 7-10%; the matching frees). The general-purpose heap also scatters consecutive orders' nodes, which is part of the miss cost in `_Find_last` and in the first touch of a cancelled order's node (5-7%).

**Hypothesis.** A free-list pool per node type (chunks of nodes handed out and reused in LIFO order) makes each allocation a pointer swap, and keeps recently freed nodes hot and consecutive orders adjacent, so both the allocation share and part of the miss share fall. Implemented as an allocator type for `std::list<Order>` and the index, so the containers and their iterator-stability guarantees stay.

**Should improve.** Every `core` workload: 15-25% beyond experiment 4, with the larger end on `cancellation-heavy` and `deep-book` if locality improves as hoped. `small-book` isolates the pure allocation saving (no misses to recover), so its gain is the floor and the large-book gain minus it is the locality effect.

**Correctness risks.** Medium to high. `allocation_failure_tests` fails each allocation in turn and expects the book sound after each; a pool allocates in chunks, so failures move to chunk boundaries, and the test's contract ("`addOrder` rolls back, `cancelOrder` fails before removing anything") must hold at those points, which it will only if the pool never allocates inside a step the contracts declare non-throwing (`cancelOrder` frees but never allocates, so a pool that allocates only on acquire is safe there). `OrderBook` is move-only because the index holds list iterators; a pool owned by the book must stay valid across a move, so it has to live behind a stable pointer or the allocator must propagate on move; getting this wrong leaves every iterator dangling after `OrderBook(OrderBook&&)`. Pool memory is retained until the book is destroyed, which changes the benchmark's allocator-state caveat and the memory footprint. The allocator type appears in `std::list<Order>::iterator`'s type, so `OrderLocation`, `OrderBookTestAccess` in every test, and `PriceLevel`'s friends change with it.

**Readability costs.** High. An allocator type in `order_location.hpp`, `price_level.hpp`, and `order_book.hpp`, a pool implementation with its own tests, and container types that no longer read as `std::list<Order>` and `std::unordered_map<OrderId, OrderLocation>`. Type aliases contain the spread but every test's `OrderBookTestAccess` names the iterator type.

**Measurement.** `allocation_profile`: 0 allocations per resting submit and cancel in steady state (chunk acquisitions appear as rare large allocations in the histogram); the sampling report's `PriceLevel::addOrder` and `restOrder` heap rows vanish and `_Find_last` self time falls if locality improved; `small-book` `core` rises by at least 10% and the large-book workloads by at least 15%; `allocation_failure_tests` passes in Release; the move-construction tests (`order_book_tests`) and every `BookState` comparison pass.

## 7. Aggregate-quantity maintenance

**Observed bottleneck.** None. `PriceLevel::quantity` is one 64-bit add or subtract per add, fill, and remove, on a field in the level node that the same operation already touches for the list's head pointer. No sampling report charges a measurable share to it (`PriceLevel::addOrder`'s self time, 0.5-1%, is the list insert; `fillOldestOrder`'s, under 1%, is the fill).

**Hypothesis.** The null hypothesis is the point of this experiment: maintaining the aggregate costs nothing measurable, and computing it on demand instead (summing the queue in `snapshot` and `getAggregateQuantity`) would gain nothing on the core path while making `snapshot` O(N) in resting orders. The experiment exists to check the method: a change the profile says is irrelevant should measure as irrelevant, and if it does not, the measurement protocol or the profile is wrong.

**Should improve.** Nothing. Prediction: every `core` workload within its base-against-base spread; `replay` kinds slower, since their final full-depth snapshot would sum every order.

**Correctness risks.** Low in the throwaway (the invariant checker already recomputes the sum independently), but the on-demand version changes `snapshot`'s complexity, which the replay format's full-depth snapshot relies on, and the `uint64_t` aggregate exists so that many maximal orders at one price cannot wrap; a sum on demand must keep that width. Not merged either way.

**Readability costs.** None if the maintained aggregate stays, which it should.

**Measurement.** `core` throughput within noise on every workload (a measured difference beyond the spread is a finding about the protocol, not about the aggregate); `replay` slower by the snapshot's cost on `deep-book` (2,000 levels, 100,000 orders). Recorded as a negative result and the current design kept.

## 8. Replace the index container, and later the level map

**Observed bottleneck.** The index lookup's self time rises from 4% (`small-book`, index in L2) to 20-25% (the three large-book workloads) with nothing else changing in the instruction stream, at the two loads of `xhash:1588-1596` (the bucket entry, then the node's key): `std::unordered_map` spends two to three dependent cache misses per lookup on a 2 MiB bucket array and nodes scattered by the heap, hashes every key with FNV-1a (which also destroys the locality of sequential IDs), rehashes on `erase(iterator)`, and walks every node to grow. The level map walk (10-12% on `deep-book` and `cancellation-heavy`) is the same pattern in a tree.

**Hypothesis.** An open-addressed flat table of `{id, location}` entries (linear probing, a cheap multiplicative hash, backward-shift deletion so cancellation churn leaves no tombstones) makes a lookup one cache line in the common case and an erase a probe plus a shift, with no node allocation at all. This is the container change the profile justifies (`PROFILING.md`'s hypothesis 2); the level map's replacement by a sorted contiguous array of levels is the same argument at smaller stakes and comes after, if at all.

**Should improve.** `deep-book`, `cancellation-heavy`, and `mostly-resting` `core` by 25-35%, with their coefficient of variation falling toward `small-book`'s; `mostly-resting`'s rise from 300 to 440 ns per command between the standard and extended sizes should shrink. `small-book` and `matching-heavy` are the controls and should move by only the allocation saving (a few percent), since their tables are already in cache. Experiment 2(b)'s slope and experiment 6's locality effect are the evidence this experiment requires before it is started; if they show the index's cost is not misses, this experiment is cancelled.

**Correctness risks.** High. A hand-written hash table replaces a standard one in the structure every command and every invariant depends on: probing and deletion bugs corrupt the index silently, and the invariant checker's index pass (`orderIndex.find`, iteration over every entry, `size`) must be rewritten for the new type and must still never dereference a stale entry's iterator. Eight test files reach the index through `OrderBookTestAccess` with `std::unordered_map`'s API (`at`, `emplace`, `erase`) to read resting orders and to plant wrong, missing, and stale entries; they all change. `allocation_failure_tests` meets a table that grows by one large allocation; growth must be exception-safe (build the new array, then swap). IDs are arbitrary 32-bit values chosen by callers, so the hash must not assume they are sequential, and a probe sequence must terminate on a full table (the load factor must be bounded with growth). The randomized harness and its saved regressions are the strongest tests for this change and should run on every seed and profile before it is merged, plus new unit tests for the table alone.

**Readability costs.** High. About 150-200 lines of hash-table code in the engine, a second place where iterator and handle stability has to be reasoned about, and the loss of `std::unordered_map`'s familiarity. The index's description in `CLAUDE.md` and `order_book.hpp` changes. The level map's replacement would also have to drop `PriceLevel`'s `const` members (a sorted array needs move-assignable elements) and keep `OrderLocation` referring to levels by price, since array elements move; that cost is why it comes last.

**Measurement.** The sampling report's `_Find_last`, `_Forced_rehash`, and `_Fnv1a` rows disappear and the new table's probe shows a self time under 10% on `deep-book`; `allocation_profile` shows no per-command index allocation and growth as rare large allocations; `deep-book` and `cancellation-heavy` `core` medians rise by at least 20% in interleaved sessions with a lower coefficient of variation; `small-book` within a few percent; the full gate, every randomized profile at several seeds, and the rewritten `invariant_tests` pass.

## 9. A replay tokenizer that does not allocate

**Observed bottleneck.** Parsing is 53-76% of the replay path and 610-800 ns per command on its own, against 160-450 for the engine: `splitRecord` grows a `std::vector<std::string>` through five reallocations for six fields and `readLine`'s `std::getline` allocates the line buffer (six allocations and six frees per record, 710 bytes, counted exactly) and fetches characters one at a time through the stream buffer's virtual interface (`snextc`, 7-11% self). `parseRow`, the actual validation, is 7-10%.

**Hypothesis.** Tokenizing in place (six `std::string_view`s into one reused line buffer, or into the in-memory text) and reading lines by scanning the buffer for the line ending rather than through `getline` removes all six allocations and most of the stream cost, leaving `parseRow`'s digit loops as the parsing cost. The format's checking order, error codes, and messages are independent of how fields are cut from the line, so the parser's observable behavior on every sample in `tests/replay/` stays byte-identical.

**Should improve.** Every `replay` kind, by 40-50% of the replay path's time (parsing from 600-800 ns to well under 200 per command); the `core` kinds are the controls and must not move at all, since the engine is untouched. The gap between `replay` and `core` in `BENCHMARKS.md` (580-650 ns) should shrink to roughly the engine-independent remainder of event collection and dispatch.

**Correctness risks.** Medium. `CsvRow`'s `std::vector<std::string> fields` is part of the parser's public interface (`splitRecord` returns it; `parseRow` takes it; `replay_parser_tests` build rows), and `ParseError` copies field text into its `value`, which must still work when fields are views into a buffer that the reader reuses (copy at the error, which is the cold path). Line-ending handling (`LF`, `CRLF`, a last line without an ending, a `CR` elsewhere staying in its field) is specified and tested by the malformed samples and must be reproduced exactly by a hand-written line scanner; `getline`'s stream-state subtleties (the `eof` rule in `readLine`) go away with it, which is a simplification but one that needs the same tests. A record longer than the reusable buffer must still parse (grow the buffer; the spec sets no line length).

**Readability costs.** Medium. A `string_view`-based row type is as readable as the current one, but the reader gains explicit buffer management in place of `std::getline`, and the parser's two layers (reader and `parseRow`) share a lifetime rule (views are valid until the next line is read) that has to be stated in `replay_parser.hpp`. The spec's statement that the parser never sees an `OrderBook` or opens a file is unaffected.

**Measurement.** `allocation_profile`'s parse stage reports 0 allocations per record and its one-run time under 200 ns per command; the sampling report's `splitRecord` heap rows and `snextc` row vanish and `readReplayInput`'s inclusive share falls below the engine's; every `replay_parser_tests` and `replay_engine_tests` expectation and every `replay_cli_*` expected file passes unchanged; `replay` medians rise by at least 30% on all five workloads with `core` inside its spread.

## Results

Each experiment's outcome, in the order run. Comparison summaries are in [`benchmarks/results/2026-10-26-experiments/`](../benchmarks/results/2026-10-26-experiments/). The base binary throughout is the Release build of commit `0677c93`, whose engine code is the baseline's. Every kept change passed the correctness gate before it was committed: the whole suite in Debug (where every public command also checks the whole book's invariants) and Release, the invariant and invariant-boundary tests, and the same suite under MSVC's AddressSanitizer in Debug and Release. UBSan is not available with MSVC, and no GCC or Clang toolchain is installed on this machine, so the UBSan configurations run only in CI.

### Experiment 1: remove unnecessary copies and no-op work. Kept, reduced.

Tried: the `SubmissionResults` constructor's `generatedTrades = {}` replaced by a member-initializer list; `restOrder`'s rollback rewritten with nested `try` blocks instead of a `std::optional<OrderLocation>`; `snapshot` moving its summary vectors into the result.

| Workload, core | All three changes | Without the `restOrder` rewrite (kept) |
|---|---:|---:|
| `small-book` | 1.017 (1.013-1.027), 94% of pairs faster | 1.019 (1.016-1.048), 88% |
| `matching-heavy` | 1.016 (1.003-1.046), 75% | 1.009 (0.984-1.030), 69% |
| `deep-book` | 1.034 (1.013-1.040), 81% | 1.007 (0.989-1.012), 56% |
| `cancellation-heavy` | 1.019 (0.982-1.056), 62% | 1.031 (0.989-1.062), 69% |
| `mostly-resting` | 1.044 (1.000-1.071), 69% | 1.030 (0.973-1.068), 62% |

Median ratio experiment/base, bootstrap 95% interval, and share of pairs in which the experiment was faster. Every `replay` row is between 0.999 and 1.024.

- **Outcome.** About 2% on `small-book`, consistent across pairs, and nothing distinguishable from noise elsewhere. The plan predicted 2-4% on `small-book` and `matching-heavy`; `small-book` is at the bottom of that range and `matching-heavy` did not show it. Both are at the protocol's resolution.
- **The `restOrder` rewrite was reverted.** The independent review found it behavior-preserving but judged it performance-neutral, since table-based exception handling makes a `try` block free on the happy path and the `optional` only saved a flag write, and slightly harder to scan, with a second handler and a nesting level. Without it the results are the same within their intervals, so it bought nothing measurable for a small readability cost.
- **Kept** the constructor and `snapshot` changes. They shorten the code, and the review confirmed both are equivalent. `snapshot` runs once per replay and never inside the core timed region, so it is a cleanup, not a speedup.
- **What the first measurement got wrong.** The whole-session protocol reported +3.5% on `matching-heavy` in all three sessions. The base-against-base run showed that the slot alone was worth +9% there, so that number meant nothing. This is why the protocol changed.

### Experiment 2(b): the order index's load factor. Diagnostic, nothing kept.

Throwaway builds set the index's `max_load_factor` to 0.25 and to 4.0 (the default is 1.0). Each was compared with the current code on `core` only, 12 pairs.

| Workload, core | 0.25 (bucket array 4x larger) | 4.0 (bucket array 4x smaller, longer chains) |
|---|---:|---:|
| `small-book` (index in cache) | 1.012 (1.008-1.016) | 0.925 (0.918-0.929) |
| `deep-book` | 0.977 (0.950-1.012) | 0.917 (0.886-0.967) |
| `cancellation-heavy` | 0.914 (0.891-0.935) | 0.911 (0.881-0.922) |

- **The larger table** helps the book whose index is in cache, by 1%, and slows `cancellation-heavy` by 9%. A bigger table costs nothing in instructions, so that slowdown comes from memory: this supports the profile's reading that the index's cost on large books is cache misses.
- **The smaller table** is 7-9% slower everywhere, cached book included. Longer chains cost instructions even in cache, so this direction does not separate misses from instructions. The plan predicted it would be no slower on the large books; that prediction was wrong.
- **The default, 1.0, is the best of the three.** No change is proposed. The evidence for misses is the asymmetry in the first column, not a clean slope.

### Experiment 2(a): reserve the order index. Kept.

`OrderBook::reserve(n)` is a capacity hint that calls `unordered_map::reserve`. The benchmark runner calls it before each run's preload, untimed, with `expectedBookSize`: the larger of the preload and the predicted final count. The hint is recorded in the JSON as `reserved_orders`. Before, `allocation_profile` counted 7 rehashes in `mostly-resting`'s timed commands at the standard size; now it counts 0, and no other workload rehashed before or after.

| Workload, core | Standard size (16 pairs) | Extended size (8 pairs) |
|---|---:|---:|
| `mostly-resting` | 1.028 (0.994-1.085), 69% | **1.195 (1.160-1.222), 100%** |
| `deep-book` | 0.974 (0.933-0.991), 19% | 1.018 (1.009-1.030), 88% |
| `small-book` | 0.977 (0.962-0.996), 12% | |
| `matching-heavy` | 1.015 (0.998-1.029), 62% | |
| `cancellation-heavy` | 0.991 (0.927-1.028), 50% | |

The `replay` rows at the standard size are 0.990-1.001.

- **The standard-size prediction failed.** The plan predicted 5-10% on `mostly-resting` and measured 3%, inside the noise floor. With seven rehashes over 100,000 commands there was little to remove.
- **The extended-size gain is larger than predicted.** The plan said up to 11%; the measurement is 19.5%, and every pair was faster. A profile of the new build at the extended size accounts for it. The rehash path, its own allocations included, was 13.9% of the old profile's samples, not the 11% self time the plan used. Every other part of inserting an order also became about 25% cheaper: the list insert, the index emplace, and the rest of `restOrder`. A plausible cause is that each rehash walked all of the index's up to 860,000 nodes and evicted the working set, but the profile does not prove it.
- **The controls are inside the noise floor.** `deep-book`'s index has the same number of buckets with and without the hint, so its -2.6% at the standard size cannot come from this change. `small-book`'s table did change, from 512 buckets (the index grows eightfold below 512) to 256, so its -2.3% has a possible mechanism. The difference is under the floor and was not pursued.
- **What it changes about the benchmark.** The gain exists only for a caller that calls `reserve`; a caller who does not sees no difference. `mostly-resting` now measures growth into a pre-sized index, so the baseline's `mostly-resting` numbers, especially the extended 440 ns per command, are no longer comparable. `benchmarks/README.md` says so.
- **Review.** The independent review found the method safe: `reserve` either succeeds or leaves the index unchanged, and a rehash never invalidates the list iterators the index holds. It required four fixes before keeping, all made:
  - the hint is recorded in the results, as the plan had demanded;
  - the header comment also names `std::length_error`;
  - a test assertion that relied on MSVC never shrinking a table was rewritten to what the standard guarantees;
  - the runner exports the helper instead of the allocation counter copying it.
- **Documentation.** `CLAUDE.md` names `reserve` as the one non-const public member without an `InvariantBoundary`, since it changes nothing the book reports.

### Experiment 3: reduce repeated lookups. Reverted; a test kept.

**Ceiling.** A throwaway build deleted `validateNewOrder`'s duplicate-ID lookup. That is not correct code, and was built only to bound what removing the second index walk per resting submit could gain.

| Workload, core | First run (12 pairs) | Recheck (24 pairs) |
|---|---:|---:|
| `deep-book` | 1.066 (1.004-1.110) | 1.028 (1.013-1.061) |
| `cancellation-heavy` | 1.049 (1.034-1.067) | 1.029 (1.002-1.107) |
| `matching-heavy` | 1.037 (1.015-1.049) | |
| `small-book` | 1.018 (1.013-1.034) | |
| `mostly-resting` | 1.020 (0.993-1.062) | |

**Design.** The first run's ceiling cleared the plan's 3% threshold, so a design was built. The plan's provisional-entry idea would have cost `matching-heavy` an extra allocation and free on each of its many fully filled submits. So the design was narrowed to submits that do not cross the book, which rest whole. For those, one `try_emplace` both refused a duplicate and created the index entry, with a placeholder location filled in once the order was queued and erased on any exception. Crossing submits kept the existing path. `restOrder` was split into queue and index steps to share code.

| Workload, core | First run (16 pairs) | Recheck (24 pairs) |
|---|---:|---:|
| `deep-book` | 0.972 (0.950-1.006) | 1.018 (0.997-1.025) |
| `cancellation-heavy` | 0.997 (0.951-1.013) | 1.004 (0.992-1.029) |
| `mostly-resting` | 1.034 (1.018-1.074) | |
| `small-book` | 1.012 (1.002-1.017) | |
| `matching-heavy` | 1.005 (0.992-1.016) | |

- **Outcome: reverted.** On the recheck the ceiling itself fell to about 3%, the noise floor. The design's gain on its target workloads cannot be told apart from zero in either run. The only clear gain was `mostly-resting`'s 3%, at the floor. MSVC's `emplace` already looks the key up before it allocates, so the design and the ceiling both make one probe per resting submit, and there was never more than the ceiling's few percent to recover.
- **The design was correct**, by the independent review. It refused duplicates in the same order, built the result before resting, and left no placeholder behind a throw. It would have carried costs had it been kept:
  - the index held, for a moment within the command, an entry whose iterator pointed at nothing, a hazard for any future code that reads the index there;
  - the duplicate check lived in three places;
  - when allocating the index node failed, the submission now consumed no sequence number, which the contract in `CLAUDE.md` would have had to describe.
- **What the plan got wrong.** It predicted a 2-5% ceiling; the first run said 5-7% and the recheck said 3%. Building the design rested on the first run alone. The review pointed out that the deep-book ceiling moved from 1.066 to 1.028 between sessions, a shift as large as one run's interval: a single run's interval overstates its certainty. Later experiments recheck a result near the floor before acting on it.
- **Kept: a test.** `testRestingSubmitRollsBack` in `allocation_failure_tests` fails each allocation in turn for a submission that rests without trading, into an existing level and into a new one. It requires the book to be left unchanged and the sequence number consumed. No such test existed. Deliberately breaking the design's cleanup made it fail. At the engine's committed code it guards `restOrder`'s rollback, and it would guard any future resting fast path. The reverted engine diff is kept in `benchmarks/results/2026-10-26-experiments/exp3-reverted.diff`.

### Experiment 4: inline storage for results' events and trades. Kept.

A header-only `SmallVector<T, N>` (`include/order_book/small_vector.hpp`) holds trivially copyable elements, `N` of them inside the object and the rest on the heap. `SubmissionResults` holds its trades in a `SmallVector<Trade, 4>` and its events in a `SmallVector<BookEvent, 5>`. `CancellationResult` holds its one event in a `SmallVector<BookEvent, 1>`. `matchOrder` builds its trades in the same type, still making room before each execution. The getters return these types. They iterate, index, compare equal with a `std::vector`, and bind to `std::span`. A `std::vector` copy is an explicit `toVector()`.

Exact allocations per command at the standard size (`allocation_profile`):

| Command kind | Before | After |
|---|---:|---:|
| submit that rests without trading | 3.0 | 2.0 |
| submit that trades, then rests the remainder | 6.0-6.2 | 4.0-4.2 |
| submit filled completely | 2.0-2.1 | 0.0-0.2 |
| cancel | 1.0 | 0.0 |

The fractions are sweeps of more than four trades, whose trades and events go to the heap.

| Workload | core (16 pairs) | core, final build (12 pairs) | replay (16 pairs) |
|---|---:|---:|---:|
| `small-book` | 1.183 (1.177-1.206), 100% | 1.193 (1.177-1.206), 100% | 1.040 (1.033-1.047) |
| `matching-heavy` | 1.191 (1.185-1.204), 100% | 1.177 (1.164-1.189), 100% | 1.046 (1.042-1.056) |
| `deep-book` | 1.161 (1.140-1.183), 94% | 1.157 (1.133-1.248), 100% | 1.050 (1.043-1.057) |
| `cancellation-heavy` | 1.190 (1.158-1.230), 100% | 1.147 (1.122-1.218), 100% | 1.035 (1.023-1.040) |
| `mostly-resting` | 1.086 (1.079-1.122), 94% | 1.129 (1.083-1.165), 100% | 1.032 (1.023-1.036) |

The base is experiment 2's code. The final build includes the review's fixes.

- **Outcome.** The plan predicted 15-25% on the cached workloads and 8-12% on the large-book ones. The cached workloads gained 18-19%, inside the prediction. The large-book workloads did better than predicted: 15-19% on `deep-book` and `cancellation-heavy`, and 9-13% on `mostly-resting`. The prediction assumed the allocation's cost was the same everywhere and only its share changed. The heap on a large book is itself slower, so removing calls to it saves more there. The replay path gained 3-5%: the same saving, set against parsing's larger cost.
- **Readability cost.** One 190-line container with its own tests (`small_vector_tests`), and new return types on two getters. Every caller that copied a result into a `std::vector` now says so with `toVector()`, which changed about 25 test lines. The replay engine, the CLI, and one test helper now take `std::span`.
- **Review.** The independent review found no bug in the container's main paths or in the engine's contracts. It required these fixes before keeping, all made:
  - **Implicit conversions removed.** The first version converted implicitly to `std::vector`, which would let a copy happen unseen in a change whose purpose is removing allocations, and also had a redundant conversion to `std::span`. A `std::span` binds to the container as a range anyway.
  - **Hardening.** Element accessors gained bounds asserts, the inline storage is reached through `std::launder`, and the header includes `<new>`.
  - **The aliasing test now tests aliasing.** As first written it grew from inline storage, which is never freed, so it could not fail. It now pushes an element while full on the heap. With the copy-before-grow line deleted, AddressSanitizer reports a heap use-after-free.
  - **The strong guarantee is tested.** The container's test replaces `operator new` to make growth fail, and checks that nothing changed. Further cases cover copying a heap vector that fits inline and an inline capacity of 1.
  - **A long sweep is tested under allocation failure.** `allocation_failure_tests` gained a six-trade sweep, the only case where matching now allocates. Its first allocation is the trades vector growing after four executions already stand, and each failure leaves those executions standing and no remainder resting.
  - **`cancelOrder` is pinned allocation-free.** It no longer allocates, so the old test, which required at least one injected failure, could not run. It is replaced by a test that `cancelOrder` never allocates, whether it cancels or refuses. `CLAUDE.md` records this as part of `cancelOrder`'s contract.
- **Not verified here.** GCC and Clang were not available on this machine, so neither their `-Werror` builds nor UBSan has compiled this container. The review named GCC's array-bounds and maybe-uninitialized warnings as a plausible risk for inline-buffer containers. CI's Linux jobs are the first place it will be checked.

### Experiment 5: a level handle in `OrderLocation`. Kept.

Each index entry also holds a `PriceLevel*` to the order's level, set when the order is queued. `cancelOrder` follows it instead of searching the side's map, and erases the level by key only when the cancellation empties it. Before mutating anything, `cancelOrder` throws if the handle does not name a level at the entry's price and side. `checkInvariants` compares each entry's handle with the level it checks the order against, without following it. The measurement was taken first on a variant that differed only in giving the handle a null default. It cleared the floor on its target, so the variant was finished into the design rather than discarded.

| Workload, core | Standard size | Recheck | Extended size (8 pairs) |
|---|---:|---:|---:|
| `cancellation-heavy` | 1.057 (1.007-1.135), 75% | 1.049 (1.010-1.090), 79%, 24 pairs | **1.067 (1.043-1.098), 100%** |
| `deep-book` | 1.026 (0.987-1.048), 62% | 0.996 (0.965-1.053), 50%, 24 pairs | |
| `mostly-resting` | 0.982 (0.927-1.012), 31% | 0.971 (0.945-1.014), 33%, 24 pairs | 0.995 (0.980-1.015), 38% |
| `small-book` | 1.022 (1.009-1.039), 100% | | |
| `matching-heavy` | 0.995 (0.989-1.007), 38% | | |

The standard size is 16 pairs. The base is experiment 4's code.

- **Outcome.** About 5-7% on `cancellation-heavy`, repeated in three measurements and at both sizes, inside the predicted 3-6%. The plan also predicted 3-6% on `deep-book`, and that did not appear in either measurement: deep-book cancels less, and when it does, its book's misses elsewhere dominate the map search. The handle makes each index node 8 bytes larger, which is the plausible cost for `mostly-resting`, whose index is the largest. Its three measurements all lean slightly negative, but none excludes 1, so it is recorded as no measurable loss rather than as none.
- **Readability cost.** About 15 lines in the engine, plus a check and tests. The handle lives exactly as long as the entry's list iterator: a level is erased only once empty, after every order in it is unindexed, and `std::map` never moves its elements, even when the book is moved. So the new field adds no new lifetime rule.
- **Review.** The independent review found no correctness problem, and judged the guard strictly safer than before. A corrupted price that named another live level used to pass a foreign iterator to the level's erase; now any mismatch throws. Its verdict was a weak keep, conditional on `mostly-resting` showing no real loss on a recheck, which is the recheck in the table. It asked for these changes, all made:
  - `CLAUDE.md`'s two descriptions of the guard are corrected, and so is the exception text.
  - `invariant_tests` now checks the detail text of the new level check, so deleting that branch would fail a test.
  - `cancellation_tests` has a test that a null handle and a handle to another level make `cancelOrder` throw without changing the book. It runs only without automatic invariant checks, since with them the command's boundary aborts first.

### Experiment 7: aggregate-quantity maintenance. A null result, as predicted; nothing kept.

A throwaway build stopped maintaining each level's cached aggregate. It removed the add or subtract in `addOrder`, `fillOldestOrder`, and `removeOrder`, and summed the queue whenever the aggregate was asked for. It was compared with experiment 5's code, 16 pairs.

| Workload | core | replay |
|---|---:|---:|
| `small-book` | 1.003 (0.993-1.006) | 1.004 (1.002-1.009) |
| `matching-heavy` | 0.994 (0.985-1.021) | 0.998 (0.994-1.006) |
| `deep-book` | 1.022 (0.957-1.041) | **0.950 (0.940-0.952)**, 0% of pairs faster |
| `cancellation-heavy` | 1.015 (0.970-1.031) | 0.989 (0.981-0.999) |
| `mostly-resting` | 1.003 (0.990-1.034) | **0.953 (0.945-0.971)**, 6% of pairs faster |

- **Outcome.** Exactly the plan's prediction. The core path is within noise on every workload: maintaining the aggregate costs nothing measurable. The replay path is 5% slower on the two workloads whose final books hold about 90,000-100,000 orders, because the replay's full-depth snapshot now sums every queue. It is unchanged on the small books.
- **What it says about the method.** The protocol reported no change where the profile said there was none, and a change of the predicted sign and rough size where one was expected. This supports reading the other experiments' results within the stated ±3% floor.
- **Reverted.** The maintained aggregate stays: it is free on the hot path, and it keeps `snapshot` proportional to levels rather than orders.
