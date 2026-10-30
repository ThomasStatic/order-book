# Profiling tools

Two small programs for finding where the benchmark workloads (`../README.md`) spend their time. They are built with `-DORDER_BOOK_BUILD_PROFILING=ON` (which needs `ORDER_BOOK_BUILD_BENCHMARKS=ON`) and are not tested. The profile of the recorded baseline, and what was concluded from it, is in [`docs/PROFILING.md`](../../docs/PROFILING.md); its raw output is in [`../results/2026-10-26-profile/`](../results/2026-10-26-profile/).

## Build

Profile the same code the benchmark measures: Release optimization (`/O2 /Ob2 /DNDEBUG`), plus symbols. `/Zi` changes no code, and without `/DEBUG` the linker already applies `/OPT:REF /OPT:ICF`, so pass them explicitly to keep the same linking:

```bash
cmake -S . -B build/profile -G "Visual Studio 17 2022" \
  -DORDER_BOOK_BUILD_BENCHMARKS=ON -DORDER_BOOK_BUILD_PROFILING=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="/O2 /Ob2 /DNDEBUG /Zi" \
  -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF"
cmake --build build/profile --config Release --target order_book_benchmarks allocation_profile sampling_profiler
```

(From Git Bash, set `MSYS_NO_PATHCONV=1` first, or the `/O2` style flags are rewritten as paths.)

## `sampling_profiler` (Windows x64 only)

A sampling profiler for one process's main thread, built on dbghelp. It starts the command after `--`, and at a fixed interval suspends the target's main thread, copies its registers and stack, resumes it, and walks the copy. The walk includes inline frames, so a function the compiler inlined (nearly every small one in this code) still shows up as its own frame. Nothing runs inside the target, and the target's thread is held only while its stack is copied, a few tens of microseconds per sample; the profiler's own work runs on another core. Symbols come from the target's PDB; system DLLs have no PDB here, so an address inside `ntdll` resolves to the nearest exported name (`RtlAllocateHeap`, `RtlFreeHeap`, and sometimes an unrelated export such as `RtlUpcaseUnicodeString` for the heap's internal functions).

```bash
build/profile/Release/sampling_profiler.exe --interval-us 1000 --out results/deep-book-core \
  --root runCore --root runReplay \
  --drop preload --drop generateWorkload --drop recordFinalBook --drop renderReplayCsv --drop describeOutcome --drop ~OrderBook \
  --drop 'order_book::PriceLevel> >,0> >::{dtor}' --drop '~unique_ptr<order_book::NodePool' \
  -- ./build/profile/Release/order_book_benchmarks.exe --workload deep-book --size extended --kind core --warmup 0 --repetitions 20
```

The target's path needs its leading `./` (or a backslash path): the command line goes to `CreateProcessW` unchanged, and it does not find `build/profile/...` written with forward slashes and no prefix.

| Option | Effect |
|---|---|
| `--interval-us N` | microseconds between samples (default 500) |
| `--out PREFIX` | writes `PREFIX.txt` (the report) and `PREFIX.folded` (every stack) |
| `--root NAME` | select only samples whose stack has a frame containing NAME (repeatable; all samples if none) |
| `--drop NAME` | exclude samples whose stack has a frame containing NAME (repeatable) |

The selection above keeps the timed regions of `benchmark_runner.cpp` and drops what the runner does around them: generating the workload, the preload, rendering the CSV, the outcome check, and destroying the book (which is inside `runCore` but after the clock stops, and for a large book costs as much as the run). Since the node pool (experiment 6 of `docs/OPTIMIZATION-EXPERIMENTS.md`) the compiler inlines `~OrderBook` into `runCore`, so the destruction shows as the level maps' destructor and the pool's `unique_ptr` destructor directly under `runCore`; the last two drops remove it. Without them, `mostly-resting` charges a quarter of its samples to freeing the book. Drop names containing spaces must be quoted.

`PREFIX.txt` reports, over the selected samples: self time by module, function, and source line; inclusive time by function; time inside the heap allocator split into allocation and free and attributed to the project function that called into it and the standard-library entry point it went through; and, for `std::map`, `std::unordered_map`, `std::list`, `std::vector`, `std::variant`, and strings and streams, the inclusive time by entry point and caller. `PREFIX.folded` is the collapsed-stack format (`frame;frame;... count`, root first) that flame graph tools read.

Caveats:

- The target runs slower under the profiler (the sampler takes a core and competes for the memory system): in the baseline's profile between 5% (`small-book`) and 27% (`cancellation-heavy`) below the unprofiled throughput, the large-book workloads most. Samples are taken on a clock independent of what the target is doing, so the shares are still representative.
- The linker's identical-code folding merges functions with the same machine code, so a frame can carry the name of a different instantiation: `std::vector<BenchmarkCommand>::~vector` in these profiles is the destructor of the results' `std::vector<BookEvent>` and `std::vector<Trade>`, which has the same code.
- A sample is a single thread's user-mode stack. Time the thread spends in the kernel, in a page fault for instance, is charged to the user-mode instruction that caused it.
- One run per workload, with the sample counts in each report; a share below about 1% is within sampling noise.

## `allocation_profile`

Exact allocation counts, not samples. Global `operator new`/`delete` are replaced with counting versions, then each workload (by default all eleven, v1 and behavior) is generated as the benchmark does and run once on a preloaded book. For the core path every timed command is run on its own and its allocations, frees, and bytes are charged to its kind (a submission that rests, one that trades then rests, one that fills completely, a cancellation; for the behavior workloads, a request submission by its type and time in force, with post-only limits apart, a replace by its path (priority kept in place, moved to the back, post-only expired, or crossing), and a cancel of a resting order or of a pending stop), with a histogram of allocation sizes, which identifies the container each allocation came from. The order index is watched for growth, and its size in slots and its fill are printed. Each workload's peak live heap bytes (over what was live before its book was built) are printed too; each block carries its size in a small header for that, so the allocator's own bookkeeping differs a little from the program's. For the replay path the three stages the benchmark times (parsing the CSV, running the records, formatting the summary) are counted and timed separately.

```bash
build/profile/Release/allocation_profile.exe --size standard > allocations.txt
```

Options: `--size smoke|standard|extended`, `--seed N`, `--workload NAME` (repeatable). Run it from a Release build: a Debug build's checked iterators and automatic invariant checks allocate too and would be counted.
