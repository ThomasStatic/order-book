# The browser engine runs in a worker of its own

In a browser tab the wasm module is instantiated in a dedicated Web Worker, the engine worker, which does nothing but relay messages between the page and the engine and service the calls Emscripten proxies to it. `runEngine` runs on a pthread of its own beneath it, beside the shards' matching threads, log writers, housekeeping, roll worker, and gateway thread, all from a pool made in advance at exactly the engine's thread count. The page's main thread runs no wasm, takes no lock in shared memory, and never waits on the engine: it only posts and receives messages.

The reason is MEMFS. Every file call a pthread makes (`open`, `write`, `rename`, `ftruncate`) is proxied synchronously to the thread that instantiated the module, which services it only when it returns to its event loop or sleeps on a futex. On the page's main thread, every log write would queue behind Angular's rendering and change detection, so a busy page would reach clients as backpressure, and the page could never block on anything without stalling the engine. In a worker of its own that thread is always free, a hung engine can't freeze the page, and terminating the worker ends every pthread with it, which gives a crash a clean last resort.

## Considered options

- **The module on the page's main thread, with `PROXY_TO_PTHREAD`.** Emscripten's default shape, and one hop shorter. But the main thread still services every proxied file call, and the page's own work decides how fast the log moves.
- **WasmFS's in-memory backend in place of MEMFS.** Its file calls run on the calling thread with no proxying, which would remove the main reason for the worker. Its flag is still marked experimental in Emscripten 7.0.0, and the choice of file system belongs to the crash and sizing decisions; the worker holds either way.

## Consequences

- Nested workers are required, so the browser floor is Safari and iOS Safari 16.4, not the 15.2 that `SharedArrayBuffer` alone needs.
- Every frame between the page and the engine takes one extra `postMessage` hop, to the engine worker and back.
- The pthread pool is strict (`PTHREAD_POOL_SIZE_STRICT=2`), so a thread the count forgot aborts loudly rather than hanging a start. A forced stop, or any exit that leaves threads behind, keeps their workers, so the page ends the engine worker and starts a new one, as after a crash.
- The page's main thread is never a place to put a wait, a lock, or a synchronous call into the engine.
