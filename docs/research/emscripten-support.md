# Emscripten support for the engine's C++20 and POSIX use

Research for [issue #110](https://github.com/ThomasStatic/order-book/issues/110) on the wayfinder map [#109](https://github.com/ThomasStatic/order-book/issues/109). Read 2026-11-09.

This note covers two things. First, what the current Emscripten release supports of what this repo uses: the C++20 library, exceptions, threads, POSIX calls, sockets, file systems, the CMake toolchain, CTest under Node, sanitizers, CI, and memory. Second, every call in the repo that would not work as written under Emscripten. It states facts and recommends nothing.

Emscripten sources are the docs at emscripten.org and the `emscripten-core/emscripten` repository's `main` branch as read on 2026-11-09. A `#Lnn` line number refers to that day's `main`. A tag-pinned link would be stabler, but the line numbers were read on `main`. Citations are reference links (for example [changelog]), resolved in [Sources](#sources).

Labels used below:
- **Inferred** marks a conclusion drawn from reading code, where no doc or test states it.
- **Unconfirmed** marks something no primary source settled.

Nothing was built. There is no emsdk on the reference machine, and a first build is [#113](https://github.com/ThomasStatic/order-book/issues/113)'s.

## Facts the decisions wait on

- **Current release.** It is **Emscripten 7.0.0, released 2026-11-06**, and 7.0.1 is in development [changelog]. emsdk's `latest` is 7.0.0 [emsdk-tags]. 7.0.0 updated libc++, libc++abi, libunwind and compiler-rt to **LLVM 23.1.3**. Its entry warns that libc++ 23 drops many transitive includes, so code must include what it uses [changelog].
- **Which platform branch the repo takes.** Emscripten defines `__EMSCRIPTEN__`, `__wasm__`, `__wasm32__` and `__unix__`. It does **not** define `__linux__`, and its own test errors if it is defined [types-test]. In C++ it defines `_GNU_SOURCE` [types-test]. So:
  - every `#ifdef _WIN32 … #else` branch in the repo takes the POSIX side;
  - every `#elif defined(__linux__)` branch takes its fallback;
  - every `_GNU_SOURCE`-guarded declaration (`fallocate`, `renameat2`, `RENAME_NOREPLACE`) is visible [musl-fcntl] [musl-stdio].
- **Some declarations have no definition.** These musl modules are left out of the libc build: `signal`, `sched`, `time` and `linux`. So are these files: `syscall.c`, `fork.c`, `posix_spawn.c`, `execve.c` and, in threaded builds, `pthread_setname_np.c`. From `linux/`, only `getdents.c`, `gettid.c`, `utimes.c`, `statx.c`, `wait4.c`, `wait3.c` and `epoll.c` are put back, and from `sched/` only `sched_yield.c` [system-libs].
  - So `fallocate`, `renameat2`, `sched_setaffinity` and `pthread_setaffinity_np` are declared but defined nowhere in the shipped libc. Their only definitions are in the excluded files [gh-search-fallocate] [gh-search-affinity] [gh-search-renameat2].
  - `syscall()` is not built, and `bits/syscall.h` has no `SYS_gettid` [syscall-h].
  - `ERROR_ON_UNDEFINED_SYMBOLS` defaults to `true` [settings-js], so a call to an undefined function fails at link time. Inferred: no build was run.
- **`std::atomic<T>::wait` and `notify_*` do not use a futex under Emscripten.** libc++'s `atomic.cpp` has futex paths for Linux, FreeBSD, OpenBSD, Windows, Apple and Fuchsia. Every other platform gets the `// Baseline is just a timed backoff` branch, where `__platform_wake_by_address` is an empty function [libcxx-atomic].
  - The backoff polls for 4 µs, then yields until 64 µs, then sleeps half the elapsed time. After 128 ms it sleeps **8 ms** per poll [libcxx-backoff].
  - A waiter that has slept a while therefore notices a change up to about 8 ms late, and polls the whole time it waits.
  - `std::mutex` and `std::condition_variable` are different: they go through `emscripten_futex_wait`, which is a real `memory.atomic.wait32` on a worker and a busy-wait on the main browser thread [futex-wait].
- **Exceptions are off by default.** "By default, exception catching is disabled in Emscripten", and a thrown exception aborts [exceptions]. `DISABLE_EXCEPTION_CATCHING` defaults to 1 [settings-js].
  - The repo throws and catches as normal control flow (`Refusal`, `LogStorageError`, `std::system_error`, `std::bad_alloc`), so every target needs `-fwasm-exceptions` or `-fexceptions` at compile and link time.
  - `-fwasm-exceptions` still emits the *legacy* Wasm EH encoding by default (`WASM_LEGACY_EXCEPTIONS = true`). `-sWASM_LEGACY_EXCEPTIONS=0` selects the standard exnref encoding [settings-js].
- **The legacy JS file system and most syscalls are proxied, synchronously, to the main JS thread.** The code says "Most syscalls need to happen on the main JS thread (e.g. because the filesystem is in JS and on that thread)" and defaults every syscall to `__proxy: 'sync'` [libcore-proxy]. The docs say the JS FS "has overhead in pthreads builds because we must proxy to the main thread", and that WasmFS "has full multithreading support" [fs-api-wasmfs].
  - So with the default FS, every log write, every `poll` and every `stat` a shard or log-writer thread makes waits for the main thread.
  - A blocked main thread therefore stalls or deadlocks them: "If the main thread blocks while a worker attempts to proxy to it, a deadlock can occur" [pthreads-blocking].
- **A browser cannot listen on a socket.** `SOCKFS.listen` throws `EOPNOTSUPP` unless running under Node [libsockfs]. The default `setsockopt` returns `ENOPROTOOPT` for every option [libsyscall-sockopt]. Under WasmFS every socket syscall returns `ENOSYS` [wasmfs-syscalls].
  - So `PollSocketSet`, and everything in `tcp.cpp` past `socket()`, cannot work in a tab.
  - Only an in-memory `SocketSet` can carry the gateway there, which is what map #109 already chose.
- **File locks and flushes are no-ops.**
  - `flock` is a weak stub that returns 0: "Pretend that the locking is successful … Emscripten programs are a single process" [libc-stubs]. `fcntl` `F_SETLK` and `F_SETLKW` do the same [wasmfs-syscalls].
  - On MEMFS, `fsync` has no stream op to call and returns 0 [libwasi-fdsync]. `fdatasync` returns 0 with "the in-memory FS is already synced" [libsyscall-fdatasync].
  - `O_DSYNC` is not read anywhere in the legacy FS. Inferred from the code.
- **MEMFS is the default FS, in memory, and per module instance.** "All files exist strictly in-memory, and any data written to them is lost when the page is reloaded" [fs-api-memfs]. Its file contents are JS `Uint8Array`s on `node.contents` [libmemfs].
  - Unconfirmed: no doc says what happens to MEMFS after `abort()`. The data belong to the aborted instance's JS objects, so a crash that kills the instance takes the files with it. Inferred.
- **`abort()` does not raise `SIGABRT`.** musl's `abort()` under Emscripten calls `_abort_js()` directly [musl-abort], so a `SIGABRT` handler never runs.
  - The JS side calls `Module.onAbort`, prints `Aborted(...)`, sets `ABORT = true` in that thread's context, and throws a `WebAssembly.RuntimeError` [preamble-abort].
  - From a pthread, the error is rethrown to the main thread's `worker.onerror` [runtime-pthread] [libpthread-onerror]. `onAbort` is forwarded to workers [libpthread-onerror].
  - Unconfirmed: whether the other threads stop, and whether the module can be instantiated again after an abort.
- **Thread stacks are 64 KiB by default.** `STACK_SIZE` is 64 KiB: "There is no way to enlarge the stack". It was cut from 5 MB in 3.1.27. `DEFAULT_PTHREAD_STACK_SIZE = 0` means "defaults to STACK_SIZE" [settings-js] [settings-ref-stack].
- **Toolchain.**
  - `emcmake` on Windows passes `-G "MinGW Makefiles"` if `mingw32-make` is on `PATH`, else `-G Ninja`, and refuses otherwise, "to keep CMake from pulling in a native Visual Studio" [emcmake].
  - The toolchain sets `CMAKE_EXECUTABLE_SUFFIX ".js"` and, unless already defined, `CMAKE_CROSSCOMPILING_EMULATOR` to `node` [emscripten-cmake].
  - CMake prepends that emulator only to an `add_test` whose `COMMAND` names an executable target [cmake-add-test] [cmake-emulator]. This repo's 11 `cmake -P` script tests run `$<TARGET_FILE:…>` through `execute_process` and get no emulator. Inferred from the CMake docs.
- **Memory.**
  - `MAXIMUM_MEMORY` defaults to 2 GiB. "To use more than 2GB, set this to something higher, like 4GB" [settings-ref-memory]. wasm32's heap tops out one page short of 4 GiB [libcore-heapmax].
  - `ALLOW_MEMORY_GROWTH` with `-pthread` "causes JS accessing the Wasm memory to be slow" [pthreads-growth], and the linker warns about it [link-py-growth].
  - `size_t`, `long` and pointers are 4 bytes on wasm32. `off_t` is 64-bit [types-test] [alltypes].

## 1. C++20 library

- **Build configuration.** libc++ is built with `_LIBCPP_HAS_THREADS 1`, `_LIBCPP_HAS_FILESYSTEM 1`, `_LIBCPP_HAS_MONOTONIC_CLOCK 1` and `_LIBCPP_HAS_MUSL_LIBC 1`. Only the time-zone database is off (`_LIBCPP_HAS_TIME_ZONE_DATABASE 0`) [libcxx-config].
- **Sources compiled.** The libc++ build compiles every `src/**/*.cpp`, including `filesystem/` and `charconv.cpp`. It leaves out only `time_zone.cpp`, `tzdb.cpp` and `tzdb_list.cpp`: "Emscripten does not have C++20's time zone support" [system-libs].
- **`<charconv>`** is built, including floating-point `to_chars` for float, double and long double, and `from_chars` for float and double [libcxx-charconv]. The repo uses only integer `from_chars` (`engine_settings.cpp`, `log_storage.cpp`'s `parseLogFileName`).
- **`<span>`, `<bit>`, `<variant>`** are header-only in libc++ 23, and nothing excludes them. Inferred from the configuration above.
- **`<filesystem>`** is compiled in and sits on the POSIX calls in §4. No doc states that it works over MEMFS (unconfirmed).
  - The repo calls `std::filesystem::directory_iterator` only on Windows. On POSIX it calls `std::filesystem::remove` (`FileLogStorage::remove`), `is_directory` and `equivalent` (`network/engine.cpp`, `apps/order_book_cli.cpp`), plus `path` arithmetic.
- **64-bit `std::atomic`.** The `std::atomic<std::uint64_t>` in `DurablePosition`, `ClosedLog` and `LogFiles` is a native wasm `i64` atomic under the threads feature (unconfirmed by a doc sentence). Its `wait`/`notify` are the timed backoff described above [libcxx-atomic].
- **`alignas(64)`** on `Ring::Side` is ordinary C++ alignment and needs nothing from the platform. The repo's layout `static_assert`s (`sizeof(CommandElement) == 64`, `sizeof(ResponseRecord) == 48` in `ring.hpp`) cover types with no pointer- or `size_t`-width members, so they should hold on wasm32. Inferred; #113's build confirms.

## 2. Exceptions

| Mode | Flag | Cost per the docs | Engines |
|---|---|---|---|
| Off (default) | none, or `-fno-exceptions` | A `throw` aborts. `-fno-exceptions` also removes the support code: Box2D with `-fno-rtti -fno-exceptions` "shrinks the output by 15%" [exceptions] [optimizing] | all |
| JS-based | `-fexceptions` | "relatively high overhead, but it will work on all JavaScript engines"; can be narrowed with `EXCEPTION_CATCHING_ALLOWED` [exceptions] | all |
| Wasm EH, legacy encoding | `-fwasm-exceptions` (default `WASM_LEGACY_EXCEPTIONS=true`) | "can reduce code size and performance overhead compared to the JavaScript-based implementation" [exceptions] | Chrome 95, Firefox 100, Safari 15.2, Node 17.0 [feature-matrix] |
| Wasm EH, exnref | `-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0` | as above; 4.0.2 moved it to the LLVM backend [changelog] | Chrome 137, Firefox 131, Safari 18.4, Node 24.15 [feature-matrix] |

- **`std::set_terminate` and unhandled exceptions.** A handler set with `std::set_terminate` is not reliably called for an exception that escapes unhandled, since neither scheme does two-phase unwinding [exceptions].
- **No repo impact.** `tests/engine_start_allocation_test.cpp` sets its terminate handler only under `__linux__`, so the limit doesn't touch it.

## 3. Threads

- **Compiling and linking.**
  - `-pthread` is needed at both compile and link time. A binary cannot use threads only when they are available [pthreads].
  - `SharedArrayBuffer`, and so every pthread build, needs COOP and COEP headers in a deployed page [pthreads].
  - The minimum engines for threads are Chrome 74, Firefox 79, Safari 14.1 and Node 16.4 [feature-matrix]. The generated code's own floor is higher: `MIN_NODE_VERSION` is 18.3.0 since 5.0.6, and `MIN_CHROME_VERSION=93`, `MIN_FIREFOX_VERSION=92`, `MIN_SAFARI_VERSION=150400` [changelog] [settings-js].
- **`PROXY_TO_PTHREAD`** "your original main() is replaced by a new one that creates a pthread and runs the original main() on it" [pthreads].
- **The worker pool.**
  - `PTHREAD_POOL_SIZE` (default 0) preloads Workers. It takes a JS expression such as `'navigator.hardwareConcurrency'` [settings-ref-pool].
  - `PTHREAD_POOL_SIZE_STRICT` (default 1) warns when the pool runs out. 2 makes running out a hard `EAGAIN`, and 0 is silent [settings-ref-pool].
  - With an empty pool, a new Worker "requires returning the main event loop", so code "cannot call pthread_create and then keep running code synchronously that expects the worker to start running" [pthreads-special].
  - Since 3.1.29, pthreads "can now be safely spawned on-demand in Node.js even without a PThread pool" [changelog].
  - Inferred: when `runEngine` runs off the main thread (under `PROXY_TO_PTHREAD`), its `std::promise`/`std::future` handshakes with new threads still complete, because the main thread is free to create the Workers. But each start waits a Worker's start-up unless the pool is pre-sized.
- **Blocking on the main thread.**
  - Emscripten busy-waits there, which "can make the browser tab unresponsive" [pthreads-blocking].
  - `ALLOW_BLOCKING_ON_MAIN_THREAD` defaults to `true`, so this only warns. At 0 it aborts with "Blocking on the main thread is not allowed by default" [libpthread-block] [settings-js].
  - `runEngine` blocks its calling thread until the engine stops, so it cannot run on the main browser thread.
- **`std::thread::hardware_concurrency`.** musl's `sysconf(_SC_NPROCESSORS_ONLN)` returns `emscripten_num_logical_cores()` [system-libs]. That function returns `navigator.hardwareConcurrency` in a browser and `os.cpus().length` on Node [libatomic]. That libc++ reaches it through `sysconf` is inferred. The engine never derives its thread count from the machine (`gatewayThreads` "never derived from the machine", `engine_settings.hpp`).
- **Signals and processes.** Emscripten "does not support POSIX signals" or `fork()` [pthreads].
- **Thread limits.** The only stated limit, 20 threads in Firefox Nightly, reads as stale docs [pthreads]. No other cap was found in the source (unconfirmed). That is [#112](https://github.com/ThomasStatic/order-book/issues/112)'s question.
- **`exit` from a pthread.**
  - `exit()` on a pthread calls `exitOnMainThread` and throws `'unwind'`, "propagate the exit back to the main thread where it can decide if the whole process should be shut down" [libcore-exit].
  - If nothing keeps the runtime alive, `proc_exit` then runs `PThread.terminateAllThreads()`, sets `ABORT`, and quits [libwasi-procexit].
  - Unconfirmed: `std::_Exit` from a pthread end to end, and the effect of `EXIT_RUNTIME` (default false) [settings-ref-exit].
- **`abort()` in a pthread.** See "Facts the decisions wait on". Inferred: a whole-module abort is the only failure an `std::abort` in a shard can cause. In-tab "crash and recover" therefore means a new module instance, not a restart inside the one that aborted.
- **Allocator.** `-sMALLOC=mimalloc` is better for multithreaded `malloc`, but "larger in code size than dlmalloc" [pthreads-alloc].

## 4. POSIX calls the repo makes

Each row is something the repo calls on the POSIX branch. "Works" means implemented in the legacy JS FS / syscall layer; WasmFS is noted where it differs.

| Call | Repo site | Under Emscripten | Source |
|---|---|---|---|
| `open`, `close`, `pread`, `pwrite`, `fstat`, `ftruncate`, `rename`, `unlink`, `mkdir`, `opendir`/`readdir` (`getdents`) | `log_storage.cpp` `NativeFile`, `FileLogStorage` | Works; proxied to the main thread under the legacy FS | [libsyscall] [wasmfs-syscalls] |
| `open(dir, O_RDONLY \| O_DIRECTORY)` + `fsync(dirfd)` | `log_storage.cpp` `moveInDirectory` | `O_DIRECTORY` honoured; `fsync` returns 0 on MEMFS (inferred from code) | [libfs] [libwasi-fdsync] |
| `O_DSYNC` | `FileLogStorage::create` (`Durability::MACHINE`) | Ignored (inferred: not referenced in the legacy FS) | [libfs] |
| `fsync` | `NativeFile::flush` | Calls the stream's `fsync` if any; MEMFS has none, so 0. WasmFS memory backend `flush()` returns 0 | [libwasi-fdsync] [wasmfs-syscalls] |
| `fallocate(fd, 0, 0, n)` | `NativeFile::extend` | Declared under `_GNU_SOURCE`; `linux/fallocate.c` not built: **undefined symbol** (inferred). `posix_fallocate` is built, and the syscall supports mode 0 only, "in terms of ftruncate" | [musl-fcntl] [system-libs] [musl-posix-fallocate] [libsyscall-fallocate] |
| `renameat2(…, RENAME_NOREPLACE)` | `moveInDirectory` (under `#ifdef RENAME_NOREPLACE`) | Macro defined under `_GNU_SOURCE`; `linux/renameat2.c` not built: **undefined symbol** (inferred) | [musl-stdio] [system-libs] |
| `flock(fd, LOCK_EX \| LOCK_NB)` | `FileLogStorage::lock` | Always returns 0 | [libc-stubs] |
| `syscall(SYS_gettid)` | `thread_placement.cpp` (Linux branch only), `tests/thread_placement_test.cpp` | `syscall()` not built; no `SYS_gettid`: **compile error** where reached. `gettid()` exists | [system-libs] [syscall-h] [gettid] |
| `setpriority`, `getpriority` | `thread_placement.cpp` (Linux branch only), test | `setrlimit` gives `EPERM` (the `prlimit64` stub); `setpriority` unconfirmed | [syscall-stubs] |
| `pthread_setaffinity_np`, `sched_getaffinity`, `CPU_SET` | `thread_placement.cpp` (Linux branch only), test | `sched/affinity.c` not built: **undefined symbol** | [system-libs] [gh-search-affinity] |
| `getpid` | none in `src/` | Returns 42 | [syscall-stubs] |
| `posix_spawn`, `waitpid`, `kill` | `tests/process/child_process.cpp` | `posix_spawn` → `ENOSYS`; `wait4` → -1; `kill` other than self → `EPERM` | [libc-stubs] [syscall-stubs] [kill] |
| `pipe` | none | Works (PIPEFS, with `poll`) | [libsyscall] [libpipefs] |
| `poll` | `tcp.cpp` `Poller::wait` | Blocks properly with `PTHREADS`; proxied to the main thread | [libsyscall-poll] |
| `eventfd` | none | Not built (inferred) | [system-libs] |
| `pthread_sigmask`, `sigwait` | `apps/engine_handlers.cpp` | Built. `sigtimedwait` checks only the in-process pending set and returns `-1`/`EINVAL` at once when nothing is pending | [sigtimedwait] |
| `std::signal(SIGABRT, h)`, `raise` | `engine_handlers.cpp` `installAbortHandler`, `onAbort` | `raise` calls an installed handler synchronously; `abort()` never raises `SIGABRT` | [raise] [musl-abort] |
| `std::signal(SIGPIPE, SIG_IGN)`, `MSG_NOSIGNAL` | `prepareStopSignals`, `tcp.cpp` | Harmless: no signal is ever delivered asynchronously (inferred) | [raise] |
| `clock_gettime(CLOCK_MONOTONIC)` / `steady_clock` | gateway, log writer, matching loop | Monotonic: "all wasi clocks but realtime are monotonic" | [libwasi-clock] |
| `read(STDIN_FILENO)` | `stop_source.cpp` `readStdinToItsEnd` | Proxied to the main thread. Browser stdin is not a pipe the page can close (inferred); Node behaviour unconfirmed | [libcore-proxy] |

## 5. Sockets

- **Default emulation.** "By default Emscripten attempts to emulate such connections to take place over the WebSocket protocol", and that needs a WebSocket-to-TCP bridge on the far side [networking].
- **Listening.** `listen` throws `EOPNOTSUPP` outside Node. On Node it starts a `ws` WebSocketServer [libsockfs].
- **`connect`** marks the socket connecting and returns 0 without waiting for it to open [libsyscall-connect].
- **Socket options.** `setsockopt` returns `ENOPROTOOPT` unless `NODERAWSOCKETS` [libsyscall-sockopt]. `getsockopt` supports only `SO_ERROR`, and `shutdown` returns `ENOSYS` unless `NODERAWSOCKETS` (unconfirmed by a link of its own).
- **`NODERAWSOCKETS`** backs POSIX sockets with Node's `node:net`. It is Node-only, and "blocking sockets cannot actually block" [networking].
- **`PROXY_POSIX_SOCKETS`** proxies sockets to a native helper process (`tools/websocket_to_posix_proxy`). It needs `-pthread -sPROXY_TO_PTHREAD`, "can be slow", and "poll(), select(), and close() are not proxied" [networking].
- **WasmFS.** Every socket syscall returns `ENOSYS` [wasmfs-syscalls].

## 6. File systems

| | Legacy MEMFS (default) | WasmFS memory backend (`-sWASMFS`) | WasmFS OPFS backend | NODEFS / NODERAWFS |
|---|---|---|---|---|
| Status | Default; mounted at `/` [fs-api-memfs] | "Stable, but not yet feature-complete with the old FS"; `WASMFS=false` by default, marked experimental; tracking [wasmfs-issue] [fs-api-wasmfs] [settings-js] | As WasmFS | Node only [fs-api-noderawfs] |
| Where data live | JS `Uint8Array` per file, outside wasm memory [libmemfs] | `std::vector<uint8_t>` "a file that lives in Wasm Memory" [wasmfs-memory] | Browser OPFS, via sync access handles under pthreads [wasmfs-h] | Host disk |
| Threads | Every call proxied to the main thread [libcore-proxy] [fs-api-wasmfs] | "full multithreading support" [fs-api-wasmfs] | `wasmfs_create_opfs_backend` "cannot be called on the browser main thread" [wasmfs-h] | Legacy FS: proxied |
| `fsync` | 0, no work [libwasi-fdsync] | `flush()` returns 0 [wasmfs-syscalls] | Unconfirmed | Host `fsync` (unconfirmed) |
| Locks | Fake, always succeed [libc-stubs] | Fake [wasmfs-syscalls] | Fake | Fake |
| Other limits | — | Sockets `ENOSYS` [wasmfs-syscalls] | `unlink()`/`rename()` "fail with EIO while the file is open" [wasmfs-h] | NODERAWFS: cwd is `process.cwd()`, "not necessarily … portable between OSes" [fs-api-noderawfs] |

- **Extend cost.** `fallocate` and `ftruncate` grow a MEMFS file with `node.contents = new Uint8Array(newSize)` plus a copy [libmemfs]. The engine extends each log file to `defaultLogFileBytes`, 256 MiB (`durability.hpp`), when it makes the file. Inferred:
  - On MEMFS, each such file costs 256 MiB of JS heap, briefly more during the copy.
  - On WasmFS's memory backend it would come out of the wasm heap, whose default cap is 2 GiB.
- **The `rename` limit under OPFS.** The OPFS backend's rename failure meets the engine's write-to-`.tmp`-then-rename pattern (`log_files.cpp` `makeLogFile`). It only matters if OPFS persistence comes back into scope.
- **The repo's own in-memory storage.** `InMemoryLogStorage` (`log_storage.cpp`) is the repo's FS-free storage. It uses no syscalls, and its `Contents` survive outside a storage object (`InMemoryLogStorage(contents)`, used by `recovery_tests` to crash at every step; `docs/internals/runtime.md`).
  - `runEngine` makes a `FileLogStorage` unconditionally (`network/engine.cpp` `recoverOnThisThread`), so using `InMemoryLogStorage` would need a seam there.

## 7. Toolchain, tests, and CI

- **The toolchain file** (`Emscripten.cmake`) [emscripten-cmake]:
  - sets `CMAKE_SYSTEM_NAME Emscripten`, `UNIX 1` and `EMSCRIPTEN 1`;
  - sets `CMAKE_EXECUTABLE_SUFFIX ".js"`;
  - sets `CMAKE_CROSSCOMPILING_EMULATOR` to the `node` it finds, if not already set;
  - documents only `-G "Unix Makefiles"` and `-G "MinGW Makefiles"` in its header.
- **This repo's CMake under that toolchain.** It is not `WIN32`, so `ws2_32` isn't linked, and it is not `MSVC`, so `order_book_enable_warnings` and `order_book_enable_sanitizers` take the GCC/Clang branch. Inferred: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` then run with a 32-bit `size_t` that no native CI job uses, so expect `-Wconversion` and `-Wshorten-64-to-32`-style warnings that Linux x64 and MSVC x64 never raise. `ORDER_BOOK_WARNINGS_AS_ERRORS` would make them fatal. #113 counts them.
- **Generator on Windows.** `emcmake` refuses to let CMake pick Visual Studio. It passes `MinGW Makefiles` if `mingw32-make` is on `PATH`, else `Ninja`, else fails with "Please install ninja or mingw32-make" [emcmake]. The existing Visual Studio `build/` can't be reused; an Emscripten build needs its own directory and Ninja.
- **CTest under Node.**
  - The emulator applies to `add_test` whose `COMMAND` is an `add_executable` target, and not to the old `add_test` form [cmake-add-test] [cmake-emulator].
  - Most of the repo's `add_test` calls (105 lines in `CMakeLists.txt`) name a target and would run under `node`.
  - The 11 `add_test` lines that run `cmake -P tests/expect_failure.cmake` or `tests/replay/check_cli_output.cmake` (some inside `foreach`, so more tests than lines) call `execute_process(COMMAND "${PROGRAM}" …)` on the `.js` path, with no emulator. Inferred from the docs.
  - Tests that pass host paths (`checkpoint_tests …/tests/checkpoint`, `replay_parser_tests …/tests/replay`, and the others reading `tests/replay`) see nothing under MEMFS. They need `-sNODERAWFS=1` (Node-only: "replaces all normal filesystem access with direct Node.js operations") or a `NODEFS` mount [fs-api-noderawfs] [fs-api-memfs].
- **Node.**
  - emsdk bundles `node-24.19.0-64bit` [emsdk-manifest]; the reference machine has v24.20.
  - `worker_threads` is stable and shares memory through `SharedArrayBuffer` [node-worker].
  - The toolchain passes no Node flags. Unconfirmed in Node's own docs: that threads and Wasm EH legacy need no flag on Node 24. The minimums in §2 and §3 are below 24.
- **Sanitizers** [sanitizers] [link-py-sanitizers]:
  - UBSan is supported, including a minimal runtime.
  - ASan "fully supports multi-thread environments". The docs advise raising `INITIAL_MEMORY` to 64 MB or growth, and the linker adds 50 MiB, plus 50 MiB more with pthreads, and defaults `EXIT_RUNTIME=1`.
  - LSan runs at exit.
  - There is no TSan runtime in `system_libs.py`; only ubsan, lsan and asan [system-libs]. The exact error is unconfirmed. `ORDER_BOOK_ENABLE_TSAN` has no Emscripten meaning.
- **GitHub Actions.**
  - `mymindstorm/setup-emsdk` now resolves to `emscripten-core/setup-emsdk`. Its latest release is v16 (2026-04-08), and its README shows `@v15` [setup-emsdk].
  - Inputs: `version` (default `latest`), `actions-cache-folder` (emsdk cached with `@actions/cache`), `no-cache` and `update`.
  - The README's example also caches the system-library cache folder with `actions/cache`, keyed by version and OS [setup-emsdk].

## 8. Memory and output size

- **Initial memory.** `INITIAL_MEMORY` defaults to computed (`-1`), from `INITIAL_HEAP` = 16 MiB on top of static data and stack. It must be a multiple of 64 KiB [settings-ref-memory].
- **Maximum memory.** `MAXIMUM_MEMORY` defaults to 2 GiB, and 4 GiB is the wasm32 ceiling [settings-ref-memory] [libcore-heapmax]. Without growth, maximum equals initial.
- **Growth.**
  - With growth on, `malloc` returns null rather than aborting [settings-ref-memory].
  - With `-pthread` the linker warns, "-pthread + ALLOW_MEMORY_GROWTH may run non-wasm code slowly, consider -sGROWABLE_ARRAYBUFFERS=2" [link-py-growth].
  - `GROWABLE_ARRAYBUFFERS` needs Chrome 144, Firefox 145, Safari 26.2 and Node 26 [feature-matrix]. Node 26 is newer than the reference machine.
- **64-bit memory.** `MEMORY64` is deprecated in favour of `-m64` / `--target=wasm64`, which makes `size_t` 8 bytes [settings-js] [types-test].
- **The engine's own memory** (from `engine_settings.hpp` defaults; inferred arithmetic):
  - Each shard has, per gateway thread, a 4,096 × 64 B command ring and a 16,384 × 48 B response ring, about 1 MiB.
  - Each shard's log ring is 65,536 `LogEntry`s.
  - Book capacity defaults are 4,096 orders, 1,024 stops and 256 levels per book.
  - The default 256 MiB log file is the large term (see §6).
- **Size levers** in the docs: `-Os`/`-Oz`, `--closure 1`, `-flto`, `-sEVAL_CTORS`, `-sFILESYSTEM=0` and `-fno-rtti -fno-exceptions` [optimizing]. The last is unavailable here because the repo needs exceptions.

## Repo calls that would not work as written

"Link" and "compile" failures are inferred from the libc build lists and `ERROR_ON_UNDEFINED_SYMBOLS=true`; nothing was built.

| # | File, function | Call | What happens under Emscripten |
|---|---|---|---|
| 1 | `src/runtime/log_storage.cpp`, `moveInDirectory` | `::renameat2(…, RENAME_NOREPLACE)` under `#ifdef RENAME_NOREPLACE` | `_GNU_SOURCE` defines the macro, so the call compiles, but `renameat2` is not in libc: link error. The `EINVAL`/`ENOSYS` fallback never gets to run [musl-stdio] [system-libs] |
| 2 | `log_storage.cpp`, `NativeFile::extend` | `::fallocate(descriptor, 0, 0, size)` | Not in libc: link error. `posix_fallocate` exists. Even once linked, a default 256 MiB log file costs 256 MiB of JS heap on MEMFS, or of wasm heap on WasmFS [system-libs] [libmemfs] [wasmfs-memory] |
| 3 | `log_storage.cpp`, `FileLogStorage::lock` | `::flock(LOCK_EX \| LOCK_NB)` | Always succeeds: the directory lock never refuses, so `Kind::LOCKED` is unreachable, including for a second storage in the same module, which native refuses [libc-stubs] |
| 4 | `log_storage.cpp`, `NativeFile::flush`, `moveInDirectory`, `FileLogStorage::create` | `fsync`, directory `fsync`, `O_DSYNC` | Succeed with no effect on MEMFS: `Durability::MACHINE` and `PROCESS` behave alike, and "durable" means "in this instance's memory" [libwasi-fdsync] [libsyscall-fdatasync] |
| 5 | `src/runtime/thread_placement.cpp`, `pinThisThread`, `setThisThreadPriority` | The `__linux__` branch is skipped | Compiles, through the `#else` fallback. Any `--pin-matching`, `--pin-gateway`, `--pin-log-writers` or `--thread-priority high` refuses the start ("pinning isn't supported on this platform") [types-test] |
| 6 | `tests/thread_placement_test.cpp` | `syscall(SYS_gettid)`, `sched_getaffinity`, `CPU_*` on its non-Windows branch | Compile error (no `SYS_gettid`) and missing `sched_getaffinity`. Its expectations (a pin succeeds) also contradict #5 [syscall-h] [system-libs] |
| 7 | `src/network/tcp.cpp`, `TcpSocket::bindIpv4`, `setNoDelay`, `listen`, `connectIpv4`, `noDelay`, `shutdownSend` | `setsockopt(SO_REUSEADDR)`, `setsockopt(TCP_NODELAY)`, `listen`, `connect`, `getsockopt`, `shutdown` | In a browser, `setsockopt` returns `ENOPROTOOPT`, so `bindIpv4` throws `"setsockopt"`. `listen` gives `EOPNOTSUPP`, and `connect` returns before opening. Under WasmFS every call is `ENOSYS` [libsyscall-sockopt] [libsockfs] [libsyscall-connect] [wasmfs-syscalls] |
| 8 | `src/network/poll_socket_set.cpp`, `connectWakePair` (every constructor, including `NoListener`) | `TcpSocket::listenLoopback(0)` + `connectLoopback` | Throws for the reason in #7, so no `PollSocketSet` can be built in a tab |
| 9 | `src/network/engine.cpp`, `runEngine` (the `Engine` start, at the `gateways[0]->sockets = std::make_unique<PollSocketSet>(…)` site) | `PollSocketSet` hardwired | Every start refuses, "cannot listen on …". The `SocketSet` interface and `InMemorySocketSet` exist, but `runEngine` has no seam to take one |
| 10 | `network/engine.cpp`, `recoverOnThisThread` | `FileLogStorage` hardwired | Runs over MEMFS (with #1–#4), every call proxied to the main thread. `InMemoryLogStorage` can't be chosen |
| 11 | `network/engine.cpp`, `runEngine` as a whole | Blocks its caller until stop; joins threads; `std::future::get` | Cannot run on the main browser thread (busy-wait, deadlock with proxied FS and `poll`). Needs `PROXY_TO_PTHREAD` or its own pthread [pthreads-blocking] [libcore-proxy] |
| 12 | `include/order_book/runtime/doorbell.hpp` `Doorbell::wait`/`ring`; `network/engine.cpp`'s `gatewaysReleased`, `finalRoll`, `happenings` | `std::atomic::wait`/`notify_*` | Works, but by polling. `notify` is a no-op, and a waiter idle for over 128 ms wakes up to 8 ms late. That is each matching thread's and log writer's sleep, so an idle engine answers its first command late. It is a CPU cost, not a correctness one [libcxx-atomic] [libcxx-backoff] |
| 13 | Everywhere that throws or catches | `throw` / `catch` | Default build aborts on the first `throw`. Needs `-fwasm-exceptions` or `-fexceptions` on every target, compile and link [exceptions] |
| 14 | `apps/engine_handlers.cpp`, `installAbortHandler` / `onAbort` | `std::signal(SIGABRT, onAbort)` | Never called: `abort()` does not raise `SIGABRT`. `describeAbort`'s line ("order_book_engine: aborted: shard …") is never printed; `Module.onAbort` is the only hook [musl-abort] [preamble-abort] |
| 15 | `apps/engine_handlers.cpp`, `forwardStopSignals` | Detached thread looping on `sigwait` | `sigtimedwait` returns `-1` at once with nothing pending, so the loop spins for ever on a Worker [sigtimedwait] |
| 16 | `src/network/stop_source.cpp`, `StdinStopSource` | Detached thread on `::read(STDIN_FILENO)` | Proxied to the main thread. In a tab there is no stdin a page can close (inferred); under Node unconfirmed |
| 17 | `apps/order_book_engine.cpp`, `main` | `std::_Exit` on `FORCED_STOP` | From a pthread it goes through `exitOnMainThread` and `proc_exit`. End-to-end behaviour, and whether it ends the instance with threads still running, is unconfirmed [libcore-exit] [libwasi-procexit] |
| 18 | `apps/order_book_cli.cpp` `send`, `src/network/client_stream.cpp` | `TcpSocket::connectIpv4`, `Poller::wait(-1)` | As #7: no TCP client in a tab |
| 19 | `tests/process/child_process.cpp` | `posix_spawn`, `waitpid`, `kill` | `ENOSYS` and -1, so `engine_process_tests`, `engine_signal_tests`, `engine_end_to_end_tests` and the engine benchmark kind can't run [libc-stubs] [syscall-stubs] |
| 20 | `tests/engine_test.cpp`, `tests/gateway_socket_test.cpp` | Real loopback TCP against `runEngine` / `PollSocketSet` | As #7–#9 in a browser. Under Node only via `NODERAWSOCKETS` (unconfirmed for this code: its sockets "cannot actually block", and the code is non-blocking + `poll`) [networking] |
| 21 | `CMakeLists.txt`, the 11 `cmake -P` tests (`log_writer_abort_*`, `clean_stop_*abort*`, `roll_forward_*abort*`, `replay_cli_*`, the benchmark `EXPECTED_EXIT` tests) | `execute_process(COMMAND "${PROGRAM}")` | Runs the `.js` file directly, with no `node` [cmake-add-test] [cmake-emulator] |
| 22 | `CMakeLists.txt`, tests passing `${CMAKE_CURRENT_SOURCE_DIR}/tests/…` | Host paths as argv | Invisible under MEMFS; needs `NODERAWFS` or `NODEFS` [fs-api-memfs] [fs-api-noderawfs] |
| 23 | `CMakeLists.txt`, `ORDER_BOOK_ENABLE_TSAN` | `-fsanitize=thread` | No TSan runtime in Emscripten [system-libs] |
| 24 | Every thread `runEngine` starts | Default stack | 64 KiB per thread unless `DEFAULT_PTHREAD_STACK_SIZE` or `pthread_attr_setstacksize` raise it. No large stack arrays were found in the paths a tab would run (`PollSocketSet::linger`'s 16 KiB is the largest, and it is unused there); recursion depth is unmeasured [settings-ref-stack] |
| 25 | `-Wconversion` on all targets | 32-bit `size_t` | New narrowing warnings, fatal with `ORDER_BOOK_WARNINGS_AS_ERRORS` (inferred) [types-test] |

What does work as written, per the sources: the `order_book` core and `order_book_replay`, apart from exceptions (#13) and #25; `InMemorySocketSet` (mutex and condition variable); the SPSC `Ring`; `MatchingLoop`, `GatewayThread` and `Shard` as the in-process engine harness composes them (`tests/engine/`); and `InMemoryLogStorage`.

Threads mean the number of pthreads `runEngine` starts. That is, per shard, a matching thread and a log writer; one per gateway thread; one housekeeper; and `--roll-workers` roll threads. The `apps/` main adds a stop-signal thread and perhaps a stdin thread, and `PROXY_TO_PTHREAD` adds one for `main`. Inferred from `network/engine.cpp`. This is the number a `PTHREAD_POOL_SIZE` would have to cover to start without waiting on Workers.

## Not stated or unconfirmed

- Whether `<filesystem>` (`remove`, `is_directory`, `equivalent`) behaves over MEMFS as over a real FS.
- What happens to the other pthreads, and to `ABORT` on the main thread, when one pthread calls `abort()`. Also whether the same `.wasm` can then be instantiated again in the same tab.
- `std::_Exit` from a pthread, end to end.
- `setpriority` under Emscripten.
- `NODERAWSOCKETS` with this repo's non-blocking + `poll` pattern.
- That Node 24 needs no flags for threads and legacy Wasm EH (Node's own docs not checked for this).
- Any browser cap on threads other than the stale Firefox Nightly note: [#112](https://github.com/ThomasStatic/order-book/issues/112).
- When, if ever, WasmFS becomes the default.

## Sources

Emscripten repository files are on `main` as read 2026-11-09.

- [changelog] https://github.com/emscripten-core/emscripten/blob/main/ChangeLog.md — 7.0.0 (11/06/26), LLVM 23.1.3; 6.0.6; 5.0.6 (`MIN_NODE_VERSION` 18.3.0); 4.0.0–4.0.2 (exnref); 3.1.29 (on-demand pthreads in Node); 3.1.27 (stack 64 KiB)
- [emsdk-tags] https://github.com/emscripten-core/emsdk/blob/main/emscripten-releases-tags.json
- [emsdk-manifest] https://github.com/emscripten-core/emsdk/blob/main/emsdk_manifest.json#L764
- [types-test] https://github.com/emscripten-core/emscripten/blob/main/test/core/test_core_types.c#L8-L73
- [alltypes] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/arch/emscripten/bits/alltypes.h#L221
- [system-libs] https://github.com/emscripten-core/emscripten/blob/main/tools/system_libs.py#L1203-L1440 (musl exclusions and re-additions), L1721–1758 (libc++ sources), L2167–2227 (sanitizer runtimes)
- [syscall-h] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/arch/emscripten/bits/syscall.h
- [gettid] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/src/linux/gettid.c
- [musl-fcntl] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/include/fcntl.h (`int fallocate(int, int, off_t, off_t);` in the `_GNU_SOURCE` block)
- [musl-stdio] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/include/stdio.h#L171-L178 (`RENAME_NOREPLACE`, `renameat2` under `_GNU_SOURCE`)
- [musl-posix-fallocate] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/src/fcntl/posix_fallocate.c
- [gh-search-fallocate] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/src/linux/fallocate.c (the only definition, in the excluded `linux` module)
- [gh-search-renameat2] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/src/linux/renameat2.c (the only definition, in the excluded `linux` module)
- [gh-search-affinity] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/src/sched/affinity.c (the only definition, in the excluded `sched` module)
- [libc-stubs] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/emscripten_libc_stubs.c#L149-L185 (`flock`, `fork`, `posix_spawn`)
- [syscall-stubs] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/emscripten_syscall_stubs.c (L21 `getpid` 42; L159–183 `prlimit64`; L185–187 `wait4`)
- [kill] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/kill.c
- [raise] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/raise.c
- [sigtimedwait] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/sigtimedwait.c
- [musl-abort] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libc/musl/src/exit/abort.c
- [preamble-abort] https://github.com/emscripten-core/emscripten/blob/main/src/preamble.js#L247-L291
- [runtime-pthread] https://github.com/emscripten-core/emscripten/blob/main/src/runtime_pthread.js#L163-L169
- [libpthread-onerror] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libpthread.js#L347-L387
- [libpthread-block] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libpthread.js#L945-L957
- [futex-wait] https://github.com/emscripten-core/emscripten/blob/main/system/lib/pthread/emscripten_futex_wait.c
- [libcxx-config] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libcxx/include/__config_site
- [libcxx-atomic] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libcxx/src/atomic.cpp#L363-L380
- [libcxx-backoff] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libcxx/include/__thread/timed_backoff_policy.h
- [libcxx-charconv] https://github.com/emscripten-core/emscripten/blob/main/system/lib/libcxx/src/charconv.cpp#L41-L91
- [libatomic] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libatomic.js#L172-L176
- [libcore-proxy] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libcore.js#L2566-L2577
- [libcore-exit] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libcore.js (`exitJS`, `exitOnMainThread`; keepalive at L2028–2038)
- [libcore-heapmax] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libcore.js (`getHeapMax`)
- [libwasi-procexit] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libwasi.js#L19-L38
- [libwasi-clock] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libwasi.js#L148-L162
- [libwasi-fdsync] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libwasi.js#L523-L538
- [libsyscall] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsyscall.js
- [libsyscall-connect] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsyscall.js#L379-L384
- [libsyscall-sockopt] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsyscall.js#L459-L473
- [libsyscall-fdatasync] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsyscall.js#L574-L577
- [libsyscall-poll] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsyscall.js#L591-L676
- [libsyscall-fallocate] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsyscall.js#L1049-L1071
- [libfs] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libfs.js#L1205-L1206
- [libmemfs] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libmemfs.js#L127-L132
- [libpipefs] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libpipefs.js
- [libsockfs] https://github.com/emscripten-core/emscripten/blob/main/src/lib/libsockfs.js#L582-L601
- [wasmfs-syscalls] https://github.com/emscripten-core/emscripten/blob/main/system/lib/wasmfs/syscalls.cpp (L331–354 `fd_sync`; L1479–1524 `fallocate`; L1586–1596 locks; L1762–1877 sockets)
- [wasmfs-memory] https://github.com/emscripten-core/emscripten/blob/main/system/lib/wasmfs/memory_backend.h
- [wasmfs-h] https://github.com/emscripten-core/emscripten/blob/main/system/include/emscripten/wasmfs.h#L82-L104
- [wasmfs-issue] https://github.com/emscripten-core/emscripten/issues/15041
- [settings-js] https://github.com/emscripten-core/emscripten/blob/main/src/settings.js (`EXIT_RUNTIME` L106, `STACK_SIZE` L113, `MAXIMUM_MEMORY` L215, `DISABLE_EXCEPTION_CATCHING` L731, `WASM_LEGACY_EXCEPTIONS` L798–803, `NODERAWFS` L1059, `ERROR_ON_UNDEFINED_SYMBOLS` L1230, `PTHREAD_POOL_SIZE` L1678, `DEFAULT_PTHREAD_STACK_SIZE` L1723, `ALLOW_BLOCKING_ON_MAIN_THREAD` L1736, `WASMFS` L1866)
- [settings-ref-pool] https://emscripten.org/docs/tools_reference/settings_reference.html#pthread-pool-size-strict
- [settings-ref-stack] https://emscripten.org/docs/tools_reference/settings_reference.html#default-pthread-stack-size
- [settings-ref-memory] https://emscripten.org/docs/tools_reference/settings_reference.html#maximum-memory
- [settings-ref-exit] https://emscripten.org/docs/tools_reference/settings_reference.html#exit-runtime
- [link-py-growth] https://github.com/emscripten-core/emscripten/blob/main/tools/link.py#L463-L464
- [link-py-sanitizers] https://github.com/emscripten-core/emscripten/blob/main/tools/link.py#L648-L690
- [feature-matrix] https://github.com/emscripten-core/emscripten/blob/main/tools/feature_matrix.py#L93-L110
- [exceptions] https://emscripten.org/docs/porting/exceptions.html
- [optimizing] https://emscripten.org/docs/optimizing/Optimizing-Code.html#code-size
- [pthreads] https://emscripten.org/docs/porting/pthreads.html
- [pthreads-blocking] https://emscripten.org/docs/porting/pthreads.html#blocking-on-the-main-browser-thread
- [pthreads-special] https://emscripten.org/docs/porting/pthreads.html#special-considerations
- [pthreads-growth] https://emscripten.org/docs/porting/pthreads.html#memory-growth
- [pthreads-alloc] https://emscripten.org/docs/porting/pthreads.html#allocator-performance
- [networking] https://emscripten.org/docs/porting/networking.html
- [fs-api-memfs] https://emscripten.org/docs/api_reference/Filesystem-API.html#memfs
- [fs-api-noderawfs] https://emscripten.org/docs/api_reference/Filesystem-API.html#noderawfs
- [fs-api-wasmfs] https://emscripten.org/docs/api_reference/Filesystem-API.html#new-file-system-wasmfs
- [sanitizers] https://emscripten.org/docs/debugging/Sanitizers.html
- [emscripten-cmake] https://github.com/emscripten-core/emscripten/blob/main/cmake/Modules/Platform/Emscripten.cmake (L48 `UNIX`, L277 suffix, L367–370 emulator)
- [emcmake] https://github.com/emscripten-core/emscripten/blob/main/emcmake.py#L47-L57
- [cmake-add-test] https://cmake.org/cmake/help/latest/command/add_test.html
- [cmake-emulator] https://cmake.org/cmake/help/latest/prop_tgt/CROSSCOMPILING_EMULATOR.html
- [node-worker] https://nodejs.org/api/worker_threads.html
- [setup-emsdk] https://github.com/emscripten-core/setup-emsdk
