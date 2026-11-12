# The browser build (Emscripten)

How the repo builds and tests under Emscripten, the target that runs the whole engine in a visitor's browser tab (spec #127; decisions in #110, #113, #115–#123 and ADRs 0016–0018). This note grows with each slice of v2.2.0. Domain terms (engine worker, page store, engine client, shadow book) are in `GLOSSARY.md` under "In the browser".

## Toolchain

Emscripten **7.0.0**, exactly, locally and in CI. On this machine emsdk is at `C:/dev/tools/emsdk` and is not on `PATH`; a shell needs:

```bash
export EMSDK=C:/dev/tools/emsdk
export PATH="/c/dev/tools/emsdk/upstream/emscripten:/c/dev/tools/emsdk/node/24.19.0_64bit:/c/dev/tools/emsdk/python/3.13.3_64bit:$PATH"
```

`emcmake` uses Ninja (never a Visual Studio generator), so the build gets a directory of its own:

```bash
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DORDER_BOOK_WARNINGS_AS_ERRORS=ON
cmake --build build-wasm
ctest --test-dir build-wasm -j8 --output-on-failure
```

The Emscripten toolchain file sets `CMAKE_CROSSCOMPILING_EMULATOR` to emsdk's `node`, which CTest puts in front of every test that names a target.

## The CMake branch

The toolchain is the switch: there is no option or preset of its own. `if(EMSCRIPTEN)` near the top of `CMakeLists.txt` adds `-pthread` (real pthreads, on Web Workers) and `-fwasm-exceptions` (legacy wasm exceptions, never `exnref`, until the browser floor passes Safari 18.4) to every target's compile and link. `ORDER_BOOK_ENABLE_TSAN` or `ORDER_BOOK_BUILD_BENCHMARKS` beside it is a configure error: Emscripten has no ThreadSanitizer, and nothing is measured in wasm.

What builds: every library and `order_book_cli`, and every test executable but the six below. `order_book_engine` doesn't, since nothing can `listen` in a tab, so `apps/engine_handlers.cpp` has no wasm branch. Neither do the benchmarks, the profiling tools, or the latency harness.

At the end of the file, once every target exists, each executable gets the link settings for running under `node` in CTest: `-sNODERAWFS=1` (the host's files, so tests read `tests/` and write their scratch directories directly), `-sEXIT_RUNTIME=1`, `-sSTACK_OVERFLOW_CHECK=2` with the shipped module's stacks (`-sSTACK_SIZE=1MB` for the main thread and `-sDEFAULT_PTHREAD_STACK_SIZE=256KB`, where Emscripten's defaults are 64 KiB each, which ASan's frames outgrew once optimized), `-sALLOW_MEMORY_GROWTH=1` with `-Wno-pthreads-mem-growth` (growth makes every pthread re-check its view of memory, which is acceptable in tests only), and a strict pthread pool: `-sPTHREAD_POOL_SIZE_STRICT=2` with 16 threads. A program that asks for one thread more than its pool holds aborts loudly rather than waiting for a thread that never comes. `order_book_wasm_threads(<target> <n>)` raises one executable's pool. `order_book_cli` gets the same settings, since under Emscripten it runs only in the script tests.

## Portability, in source

Every wasm-only branch is under `__EMSCRIPTEN__`, and the wasm jobs are their only check.

- `FileLogStorage`'s rename takes the plain-`rename` branch, as on systems without `RENAME_NOREPLACE`: Emscripten's libc has no `renameat2`, so a rename there replaces. The browser's log lives in `PageLogStorage`, whose own checks require the refusal.
- `NativeFile::extend` sets the end of the file instead of calling `fallocate`, which Emscripten's libc lacks.
- `FileLogStorage::lock` takes `flock`, which Emscripten accepts without locking anything, then holds the lock in a process-wide registry by path. A module is one process that shares its files with nothing, so that registry is the whole of "one holder at a time".
- `size_t` is 32 bits on wasm32. The 14 conversions only that target saw (`-Wshorten-64-to-32`) are fixed with types or casts, never a `-Wno-` flag: 10 in the command generator, through `indexBelow`, which draws exactly what `below` does, so every pinned fingerprint and `generatorVersion` 9 are unchanged.
- `pastTheLast` in `thread_placement.cpp` is defined only where a branch uses it.
- **`Doorbell`** sleeps on `emscripten_futex_wait` and wakes with `emscripten_futex_wake`, on its own atomic's word, exactly where native calls `std::atomic::wait` and `notify_one`. libc++'s `std::atomic::wait` polls under Emscripten, waking up to every few milliseconds, so idle matching threads and log writers would never quite sleep. Every change to the atomic is still an exchange, so the lost-wake-up reasoning in `runtime.md` holds unchanged, and `wait` loops on a load until the state isn't asleep, as `std::atomic::wait` does. `matching_loop_tests`' `testARingWakesASleeper`, which hangs without the wake, runs in both wasm jobs.

## Tests under node

Every registered test runs under `node`, against the very expected files and fingerprints native uses: passing both is the byte equality, and no job diffs native output against wasm.

- **Script tests** (`replay_cli_*`, the `*_abort_*` cases, `convert`, `checkpoint`, `verify`, `cut-log`): each `add_test` passes `-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}`, empty natively, and `tests/expect_failure.cmake` and `tests/replay/check_cli_output.cmake` run `${EMULATOR} ${PROGRAM}`, since CMake puts the emulator only in front of a command that names a target.
- **Not built under Emscripten**, each needing TCP, a child process, a signal, or the engine executable: `engine_tests`, `engine_start_allocation_tests`, `gateway_socket_tests`, `engine_process_tests`, `engine_signal_tests`, `engine_end_to_end_tests`.
- `thread_placement_tests` exits 77, which its `SKIP_RETURN_CODE` shows as Skipped.
- `log_writer_tests` skips only "a rename never replaces a file" on real files, printing why.
- `node_pool_tests` sizes its size classes from its `Node`, which is 40 bytes on wasm32 rather than 48, so six sizes still claim six classes and the seventh, past the largest, goes to the heap.

## CI

Two jobs in `ci.yml`, on `ubuntu-24.04` with Ninja and `emscripten-core/setup-emsdk` pinned to Emscripten 7.0.0, with emsdk and the system libraries it builds on first use cached under `emsdk-cache`, keyed by job:

- **Emscripten Release**: invariant checks at their default, so off, as shipped.
- **Emscripten ASan+UBSan Debug**: invariant checks on.

Both build with warnings as errors, run every registered test (not only `-L threaded`, a label that keeps meaning the ThreadSanitizer and Arm jobs) and the CLI step under `node`, within the 30-minute job limit.
