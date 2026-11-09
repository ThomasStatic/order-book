# Browser limits for a threaded wasm engine

Research for [#112](https://github.com/ThomasStatic/order-book/issues/112), on the map [#109](https://github.com/ThomasStatic/order-book/issues/109). Gathered 2026-11-09. Every claim links the source that owns it. Where a vendor publishes nothing, the note says so instead of guessing. Lines marked **Inference** are this note's reading of the sources, not something a source states.

## The answer in brief

- **Who can run it.** About 97% of tracked global usage has `SharedArrayBuffer` and wasm threads ([caniuse](https://caniuse.com/sharedarraybuffer), [caniuse](https://caniuse.com/wasm-threads), StatCounter data for October 2026). Every current engine supports both. The gaps that matter are environments, not versions: **Android WebView (in-app browsers) has neither `SharedArrayBuffer` nor COOP**, and only a cross-origin-isolated page gets shared memory at all.
- **Exceptions.** Legacy wasm exception handling reaches about 96.5–97.4% of usage, and the final `exnref` form about 87–91%, because Safari got `exnref` only in 18.4. Emscripten still emits the legacy form by default.
- **Memory.** wasm32 caps one memory at 4 GiB. On 64-bit Chrome and Firefox a large `maximum` costs only address space, and creating the memory does not fail because of it. **32-bit Chrome on Android caps at 2 GiB − 64 KiB and may quietly hand back a smaller maximum than asked.** 32-bit Firefox clamps a declared maximum to 1 GiB. **iOS has the tightest limits and publishes none of them.** WebKit reserves a shared memory's whole maximum up front from a 16 GiB pool shared with every buffer in the process, and an open WebKit bug shows a 2 GiB-maximum shared memory failing on an iPhone. The native defaults (256 MiB log files, two per shard counting the next file) do not fit a phone.
- **Workers.** No spec limit applies. Firefox allows 512 per domain. The HTML spec calls workers heavyweight. Each Emscripten pthread is a dedicated worker that instantiates the whole module.
- **Background tabs.** Workers are not timer-throttled in release Chrome or Firefox, and a wasm `Atomics.wait` isn't a timer at all. But **Chrome freezes a hidden page together with its dedicated workers**: on Android after 5 minutes, and on desktop under Energy Saver when the page is CPU-heavy. **iOS suspends background tabs' processes outright.** Spinning and sleeping threads stop together and pick up where they left off. Nothing is slowed piecemeal.
- **Detection.** Check `crossOriginIsolated`, `SharedArrayBuffer` transfer, and `WebAssembly.validate` on tiny threads and exceptions modules. Then create the shared `WebAssembly.Memory` yourself, inside `try`, at the fixed size the engine needs, and hand it to the module. A `RangeError` there is the fallback signal.

## 1. Support and usage share

### Versions

| Feature | Chrome / Edge | Firefox | Safari (macOS / iOS) | Source |
|---|---|---|---|---|
| `SharedArrayBuffer` (with cross-origin isolation) | 68 desktop, 89 Android | 79 | 15.2 / 15.2 | [MDN BCD `SharedArrayBuffer.json`](https://github.com/mdn/browser-compat-data/blob/main/javascript/builtins/SharedArrayBuffer.json), [caniuse](https://caniuse.com/sharedarraybuffer) |
| COOP and COEP headers | 83 | 79 | 15.2 | [BCD COOP](https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Opener-Policy.json), [BCD COEP](https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Embedder-Policy.json) |
| COEP `credentialless` | 96 | 119 (desktop only) | none | [BCD COEP](https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Embedder-Policy.json) |
| `crossOriginIsolated` | 87 | 72 | 15.2 | [BCD `crossOriginIsolated.json`](https://github.com/mdn/browser-compat-data/blob/main/api/_globals/crossOriginIsolated.json) |
| wasm threads and atomics | 74 | 79 | 14.1 / 14.5 | [WebAssembly `features.json`](https://github.com/WebAssembly/website/blob/main/features.json), [caniuse](https://caniuse.com/wasm-threads) |
| Legacy wasm exceptions (`try`/`catch`) | 95 | 100 | 15.2 | [`features.json`](https://github.com/WebAssembly/website/blob/main/features.json) |
| wasm exceptions with `exnref` (`try_table`) | 137 | 131 | 18.4 | [`features.json`](https://github.com/WebAssembly/website/blob/main/features.json) |
| Memory64 | 133 | 134 | behind a flag (Technology Preview 251) | [`features.json`](https://github.com/WebAssembly/website/blob/main/features.json) |
| `Atomics.wait` | 68 desktop, 89 Android | 78 | 15.2 | [BCD `Atomics.json`](https://github.com/mdn/browser-compat-data/blob/main/javascript/builtins/Atomics.json) |

`features.json` was read at commit `128c381` (2026-11-02). It marks legacy exception handling as phase "inactive" and `exnref` as phase 5 (standardized).

- Shared memory needs a secure context and cross-origin isolation. Without the headers, the global `SharedArrayBuffer` constructor is hidden and `postMessage` throws for one ([MDN](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer#security_requirements)). Isolation takes `Cross-Origin-Opener-Policy: same-origin` with `Cross-Origin-Embedder-Policy: require-corp` (or `credentialless` where supported) ([web.dev](https://web.dev/articles/coop-coep)). Getting these headers on Vercel is [#111](https://github.com/ThomasStatic/order-book/issues/111)'s question.
- **Android WebView** has `SharedArrayBuffer` and COOP marked unsupported ([BCD `SharedArrayBuffer.json`](https://github.com/mdn/browser-compat-data/blob/main/javascript/builtins/SharedArrayBuffer.json), [BCD COOP](https://github.com/mdn/browser-compat-data/blob/main/http/headers/Cross-Origin-Opener-Policy.json)), so a link opened inside an Android app's in-app browser can't run the threaded engine. **Inference:** caniuse doesn't list WebView separately, so this group is hidden inside the shares below.
- On iOS every browser is WebKit: "Apps that browse the web must use the appropriate WebKit framework and WebKit JavaScript", except under an alternative-engine entitlement in the EU and Japan ([App Review Guideline 2.5.6](https://developer.apple.com/app-store/review/guidelines/#2.5.6)). So Chrome and Firefox on iPhone have Safari's limits. BCD's iOS WebView entries mirror Safari on iOS. This note did not verify that `WKWebView` in-app browsers get cross-origin isolation.
- Emscripten's default output already assumes Chrome 93, Firefox 92, and Safari 15.4 (`MIN_CHROME_VERSION`, `MIN_FIREFOX_VERSION`, `MIN_SAFARI_VERSION` = `150400`) ([settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html)). The Safari default is above 15.2, the first version with shared memory.

### Usage share

Source: caniuse's full data file ([`data-2.0.json`](https://github.com/Fyrd/caniuse/blob/main/fulldata-json/data-2.0.json)), updated 2026-11-05 and built on StatCounter global data for October 2026. It tracks 97.57% of usage, and 61.43 points of that are mobile.

- `SharedArrayBuffer`: **97.13%** supported ([caniuse](https://caniuse.com/sharedarraybuffer)). The only tracked browsers above 0.03% without it are IE 11 (0.17%) and QQ Browser (0.09%).
- wasm threads: **97.18%** ([caniuse](https://caniuse.com/wasm-threads)).
- Largest single groups: Chrome for Android 44.02%, desktop Chrome 24.96%, Safari on iOS 13.23%, desktop Edge 5.05%, desktop Safari 2.91%, desktop Firefox 2.13%, Samsung Internet 1.94%.
- caniuse has no wasm exceptions entry. This note computed it from the same agent shares, using the versions in `features.json`. Edge counts as Chrome, Opera ≥ 81 / ≥ 122 as Chromium 95 / 137, and agents whose Chromium base isn't known (Samsung Internet, Opera Mobile, and UC for `exnref`, UC for legacy) count only toward the upper bound:
  - legacy exceptions: **96.50–97.36%**;
  - `exnref`: **87.28–91.24%**. Safari on iOS below 18.4 alone is 1.17%.
- Limits of the data: caniuse lists only the current version of Chrome for Android (157) and Firefox for Android (159), so it can't split Android usage by version. It also can't separate 32-bit from 64-bit Chrome, or in-app WebViews. **Inference:** "about 97% can run threads" is the ceiling. Isolation failures, in-app browsers, and memory refusals all come off it, and none of them is measured anywhere public.

## 2. Memory

### What the spec allows

- A 32-bit memory's `min` and `max` go up to 65,536 pages (4 GiB), and a 64-bit memory's runtime size up to 262,144 pages (16 GiB). "In practice, an implementation may run out of resources for valid modules below these limits" ([JS API, Limits](https://webassembly.github.io/spec/js-api/#limits)).
- The `WebAssembly.Memory` constructor throws a `RangeError` when allocation fails ([JS API, Memory constructor](https://webassembly.github.io/spec/js-api/#dom-memory-memory)). That is the only failure signal a page gets.
- Emscripten's `MAXIMUM_MEMORY` defaults to 2 GiB, and going to 4 GiB is an explicit opt-in ([settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html), [V8 blog](https://v8.dev/blog/4gb-wasm-memory)). Pthreads with `ALLOW_MEMORY_GROWTH` make JS access to wasm memory slow ([pthreads docs](https://emscripten.org/docs/porting/pthreads.html)). `GROWABLE_ARRAYBUFFERS` is "the only way to completely remove the overhead of growable memory + pthreads", and Firefox supports it only from 154 ([settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html)).

### What each engine reserves, and whether a large maximum fails up front

**Chrome (V8).** Sources: [`wasm-limits.h`](https://chromium.googlesource.com/v8/v8/+/refs/heads/main/src/wasm/wasm-limits.h) and [`backing-store.cc`](https://chromium.googlesource.com/v8/v8/+/refs/heads/main/src/objects/backing-store.cc).

- On 64-bit, a 32-bit memory may go to 65,536 pages (4 GiB). On 32-bit builds the limit is 32,767 pages ("2 GiB − 64 KiB"), for memory64 too.
- 64-bit with the trap handler: every 32-bit memory reserves a fixed 8 GiB guard region (`kFullGuardSize32`), whatever its maximum. Only `initial` is committed. So a large `maximum` costs nothing extra, and creation fails only when the process runs out of address space. V8's UMA buckets this case as `kAddressSpaceLimitReachedFailure`.
- Without guard regions (32-bit builds): V8 reserves the maximum. If that fails, it **retries with smaller maximums**: three-quarters, half, then a quarter of the way down, and finally `initial`. So creation can succeed with less capacity than declared, and a later `memory.grow` (here, `malloc`) fails instead.
- Which Android devices get 32-bit Chrome: a Chromium developer wrote in 2022 that the 64-bit-preferred build ships to devices with at least 6.5 GB of RAM available to the kernel, which is roughly phones sold as 8 GB, and the rest keep the 32-bit build ([android-webview-dev](https://groups.google.com/a/chromium.org/g/android-webview-dev/c/KO8iNxKNF1c)). No current share of 32-bit Chrome is published.

**Firefox (SpiderMonkey).** Sources: [`WasmMemory.cpp`](https://github.com/mozilla-firefox/firefox/blob/main/js/src/wasm/WasmMemory.cpp) and [`ArrayBufferObject.cpp`](https://github.com/mozilla-firefox/firefox/blob/main/js/src/vm/ArrayBufferObject.cpp).

- On 64-bit, where address space allows, 32-bit memories are "huge memories": 4 GiB plus a guard is reserved "regardless of the memory's byteLength".
- On 32-bit, a declared maximum is clamped to max(1 GiB, initial), "to prevent applications specifying a large max ... from unintentially OOMing the browser". A memory with a maximum is reserved once, up front.

**Safari (JavaScriptCore).** Sources: WebKit `main` at 2026-11-04: [`WasmMemory.cpp`](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/wasm/WasmMemory.cpp), [`BufferMemoryHandle.h`](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/BufferMemoryHandle.h), [`BufferMemoryHandle.cpp`](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/BufferMemoryHandle.cpp), [`Gigacage.h`](https://github.com/WebKit/WebKit/blob/main/Source/bmalloc/bmalloc/Gigacage.h), [`OptionsList.h`](https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/OptionsList.h). Shipping Safari may differ.

- A 32-bit memory first tries a "fast memory": a 4 GiB virtual reservation plus a redzone. A process gets at most 8 fast memories, and **3 on iOS** (`maxNumWasmFastMemories`; iOS has no "large Gigacage").
- Without a fast memory, a **shared** memory reserves its whole maximum up front, capped at `maxGrowableBufferReservationBytes`. That cap is a quarter of the primitive Gigacage, which is **16 GiB on iOS** and 64 GiB elsewhere. The source says so itself: "A growable buffer reserves its whole maximum up front." Its FIXME says nothing bounds the total across buffers.
- Physical bytes are charged against 3 × RAM. On iOS the real ceiling is the OS memory limit for the WebContent process, which Apple does not publish.

**Field reports on iOS:**

- [WebKit bug 255103](https://bugs.webkit.org/show_bug.cgi?id=255103) (open since 2023-04-06): `new WebAssembly.Memory({initial: 196 MiB, maximum: 2 GiB, shared: true})` throws "Out of memory" on an iPhone 14 Pro Max after iOS 16.4, while `shared: false` works.
- [Emscripten #19374](https://github.com/emscripten-core/emscripten/issues/19374): pthread builds (16 MiB initial, 512 MiB maximum) hit `RangeError` after a few reloads on iOS and macOS Safari. Users reported it fixed in iOS 18.0, and the issue was closed 2024-09-23.
- [WebKit bug 281657](https://bugs.webkit.org/show_bug.cgi?id=281657): Safari retained shared wasm memory in workers across reloads. Fixed 2024-10-29 (285841@main).

### What this means for the engine's assumptions

- Native sizing: `reserve(capacityOf(...))` warms each book to 4,096 orders, 1,024 stops, and 256 levels per side. The log ring holds 65,536 records. A log file is 256 MiB by default, and Housekeeping makes each shard's next file ahead of time (`docs/internals/runtime.md`). All of that comes from one wasm memory of at most 4 GiB (2 GiB − 64 KiB on 32-bit Chrome), shared by every thread's stack and heap.
- **Log files under MEMFS are not in wasm memory.** MEMFS keeps each file's contents in a JS `Uint8Array`, and resizing a file allocates a new array of the full size and copies into it ([`libmemfs.js`](https://github.com/emscripten-core/emscripten/blob/main/src/lib/libmemfs.js) `resizeFileStorage`). With pthreads, filesystem operations are proxied to the main thread ([Filesystem API](https://emscripten.org/docs/api_reference/Filesystem-API.html)). **Inference:** with default settings, each shard's current and next log files would cost about 512 MiB of real main-thread JS memory, on top of the wasm heap. That is outside anything a phone reliably gives. The wasm build needs a much smaller `--log-file-size` (it must still be at least `leastLogFileBytes`) and smaller book capacities and ring sizes.
- **Inference:** on 64-bit desktop browsers, a 2–4 GiB maximum is harmless at creation time. On 32-bit Android Chrome it can be quietly shortened, and on iOS a 2 GiB maximum is already known to fail. A small, fixed-size memory with no growth is the setting that fails up front, where the page can catch it, and it also avoids the pthreads-plus-growth slowdown. No source gives a number that is safe on iOS. A prototype ([#113](https://github.com/ThomasStatic/order-book/issues/113)) has to measure it on real iPhones.
- Memory64 isn't an escape route: Safari has it only behind a flag ([`features.json`](https://github.com/WebAssembly/website/blob/main/features.json)), and Emscripten marks its `MEMORY64` setting deprecated ([settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html)).

## 3. Workers: how many, and what each costs

- The HTML spec sets no number. It says workers are "relatively heavy-weight, and are not intended to be used in large numbers", and are expected to have "a high start-up performance cost, and a high per-instance memory cost" ([HTML, Workers](https://html.spec.whatwg.org/multipage/workers.html#workers)).
- `navigator.hardwareConcurrency` may report fewer cores where a UA limits workers ([HTML](https://html.spec.whatwg.org/multipage/workers.html#navigator.hardwareconcurrency)). WebKit reports **4 or 8 whatever the machine has** (4 below 8 cores), and 4 or 8 under its tracking protections ([`NavigatorBase.cpp`](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/page/NavigatorBase.cpp)). So the shard count should not follow it.
- **Firefox:** `dom.workers.maxPerDomain` is 512, "effectively infinite, while preventing abuse" ([`all.js`](https://github.com/mozilla-firefox/firefox/blob/main/modules/libpref/init/all.js)). Emscripten's pthreads page still cites an old Firefox default of 20 ([pthreads docs](https://emscripten.org/docs/porting/pthreads.html)), which the current pref replaces.
- **Chrome and Safari:** this note found no documented per-page worker limit.
- **Emscripten's cost per thread.** Each pthread is a `new Worker`. The main thread posts it the compiled `WebAssembly.Module` and the shared memory, and the worker instantiates the module ("This is a heavy operation", [`libpthread.js`](https://github.com/emscripten-core/emscripten/blob/main/src/lib/libpthread.js)). Each thread's stack is `DEFAULT_PTHREAD_STACK_SIZE`, which falls back to `STACK_SIZE` (64 KiB) inside the shared memory ([settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html)). No vendor publishes a per-worker byte figure, so it has to be measured.
- **Starting threads needs the event loop.** `pthread_create` on a fresh worker can't run until the main thread returns to its event loop. `-sPTHREAD_POOL_SIZE` pre-creates workers before `main()`, `-sPROXY_TO_PTHREAD` moves `main()` off the browser thread, and `PTHREAD_POOL_SIZE_STRICT=2` turns pool exhaustion into `EAGAIN` ([pthreads docs](https://emscripten.org/docs/porting/pthreads.html), [settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html)).
- **Inference:** the engine's thread count is 2 × shards (matching thread and log writer) + 1 housekeeping + gateway threads + roll workers (`docs/internals/runtime.md`, `docs/internals/network.md`). Two shards with one gateway thread and one roll worker is 7 workers, well inside every limit above, but each one is a full module instance.
- **The browser main thread can't block.** "Only shared and dedicated worker agents allow the use of JavaScript Atomics APIs to potentially block" ([HTML, agents](https://html.spec.whatwg.org/multipage/webappapis.html#integration-with-the-javascript-agent-formalism)). `Atomics.wait` throws a `TypeError` on the main thread ([MDN](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Atomics/wait)). Emscripten turns main-thread `pthread_join`, mutex locks, and futex waits into busy-waits, which can deadlock against proxying ([pthreads docs](https://emscripten.org/docs/porting/pthreads.html)). No engine thread, and no `std::atomic::wait`, may run on the page's main thread.

## 4. Background tabs: throttling, freezing, suspension

**Timer throttling targets Window timers, not worker threads.**

- Chrome's intensive throttling (since Chrome 88; chromestatus says enabled by default from 86) applies to "a timer task" in "a Window whose top Window has been hidden for 5 minutes": chained timers then run once a minute ([chromestatus](https://chromestatus.com/feature/4718288976216064), [Chrome blog](https://developer.chrome.com/blog/timer-throttling-in-chrome-88)).
- Chrome's intervention to throttle dedicated workers in background pages is marked "No active development" ([chromestatus](https://chromestatus.com/feature/6334419736461312)).
- Firefox's budget throttling is for timeouts (`dom.timeout.*`). `dom.workers.throttling.enabled` is on only in Nightly builds ([`StaticPrefList.yaml`](https://github.com/mozilla-firefox/firefox/blob/main/modules/libpref/init/StaticPrefList.yaml), read 2026-11-09).
- **Inference:** a worker that spins, or sleeps in `memory.atomic.wait` / `Atomics.wait`, isn't throttled in a hidden tab by any of these. Any timer-driven code on the page's main thread is, for example a client pump built on `setTimeout`.

**Freezing stops the page and its workers together.**

- The Page Lifecycle spec says a frozen document's tasks don't run, and "any dedicated worker agent whose ... owning document is non-null and frozen must become blocked". It also says hidden pages are normally frozen after a grace period ([Page Lifecycle](https://wicg.github.io/page-lifecycle/spec.html)).
- In Chromium, a frozen frame freezes its dedicated workers: `DedicatedWorker::ContextLifecycleStateChanged` calls `Freeze` on `kFrozen` ([`dedicated_worker.cc`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/core/workers/dedicated_worker.cc)). The worker thread is paused through a V8 interrupt, so running JS or wasm stops at its next interrupt check ([`worker_thread.cc`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/core/workers/worker_thread.cc)). The older chromestatus entry for this still reads "developer trial" ([chromestatus](https://chromestatus.com/feature/5276568928649216), last updated 2022). The current source is what runs.
- **Chrome on Android** freezes "background pages (and associated workers)" after 5 minutes. A proposal would cut that to 1 minute, to match Android 15's background network cutoff ([chromestatus](https://chromestatus.com/feature/5386725031149568)).
- **Chrome desktop with Energy Saver on** (Chrome 133+) freezes a browsing context group that has been hidden and silent for more than 5 minutes, if a same-origin frame group in it is "CPU-intensive". There is no published threshold. The `BackgroundPageFreezeOptOut` origin trial opts a site out ([Chrome blog](https://developer.chrome.com/blog/freezing-on-energy-saver)). **Inference:** a spinning matching loop is the kind of load that test is meant to catch.
- Chrome may also discard hidden tabs. After a discard nothing runs, and the page reloads when revisited, with `document.wasDiscarded` set ([Page Lifecycle API](https://developer.chrome.com/docs/web-platform/page-lifecycle-api)).

**iOS suspends.** WebKit changed iOS to suspend background tabs' processes as soon as their critical work finishes, instead of after 8 minutes ([WebKit bug 265064](https://bugs.webkit.org/show_bug.cgi?id=265064), fixed 2024-01-11). **Inference:** all workers stop with the process, and an OS memory kill of a suspended tab loses the MEMFS state.

**What this means for spin-then-sleep and the hold** (all inference from the above):

- In a visible tab, or a hidden one that isn't frozen, the 100 µs spin window and the doorbell sleep behave as natively, apart from the OS lowering the background process's priority.
- When frozen or suspended, every thread stops, and none is slowed relative to the others. When the page thaws they continue. A response held for the durable position is still released once the log writer catches up, so the hold's ordering is safe.
- Two things do change:
  - Wall-clock time jumps by the length of the freeze. The gateway's 5 s linger and the stop grace read injected clocks, so they would see the jump.
  - A discard or an iOS memory kill ends the engine with no shutdown, the "crash the engine" case, with no recovery state if the logs were only in MEMFS.
- Spinning costs battery on phones whenever the tab is visible and idle.

## 5. Detecting support before loading the module

In order, each step cheap and synchronous except the first module compile:

1. `self.crossOriginIsolated === true` ([BCD](https://github.com/mdn/browser-compat-data/blob/main/api/_globals/crossOriginIsolated.json), [web.dev](https://web.dev/articles/coop-coep)). If it is false, `SharedArrayBuffer` is hidden or can't be posted ([MDN](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer#security_requirements)).
2. `typeof SharedArrayBuffer === "function"`, plus `new MessageChannel().port1.postMessage(new SharedArrayBuffer(1))` inside `try`. Google's `wasm-feature-detect` does this for Firefox before validating a threads module ([`threads/index.js`](https://github.com/GoogleChromeLabs/wasm-feature-detect/blob/main/src/detectors/threads/index.js)).
3. `WebAssembly.validate(bytes)` ([JS API](https://webassembly.github.io/spec/js-api/#dom-webassembly-validate)) on tiny modules:
   - threads: `(memory 1 1 shared)` with an `i32.atomic.load` ([`threads/module.wat`](https://github.com/GoogleChromeLabs/wasm-feature-detect/blob/main/src/detectors/threads/module.wat));
   - legacy exceptions: `try` … `catch_all` … `end` ([`exceptions/module.wat`](https://github.com/GoogleChromeLabs/wasm-feature-detect/blob/main/src/detectors/exceptions/module.wat));
   - `exnref`: `try_table` with `catch_all_ref`, if the build uses it.

   Validate whichever form the build actually emits. Emscripten's `WASM_LEGACY_EXCEPTIONS` defaults to true ([settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html)).
4. Create the memory yourself, inside `try`: `new WebAssembly.Memory({initial: N, maximum: N, shared: true})`, at the fixed size the build needs, and give it to the module (Emscripten's `IMPORTED_MEMORY` / `Module.wasmMemory`). A `RangeError` means fall back ([JS API](https://webassembly.github.io/spec/js-api/#dom-memory-memory)). **Inference:** with `initial == maximum` the check is exact, since V8's quiet shrinking only reduces the maximum toward `initial`, and the probe is the real allocation, not a second reservation.
5. Start the pthread pool and treat a worker `error` event, or a pool that never becomes ready, as a failure ([`PTHREAD_POOL_DELAY_LOAD`, `Module.pthreadPoolReady`](https://emscripten.org/docs/tools_reference/settings_reference.html)).

A threaded build can't fall back to one thread at run time. "You need two builds and must choose between them at runtime" ([pthreads docs](https://emscripten.org/docs/porting/pthreads.html)). So the fallback is a second artifact or no live engine, which is the map's open "fallback" item.

## Not settled here

- The largest shared memory that reliably works on current iPhones and iPads, and on 32-bit Android Chrome. No vendor publishes it. It needs a probe page on real devices.
- The measured cost per pthread worker in each engine.
- How much of the ~97% is in-app WebViews (Android, where the engine can't run) or `WKWebView` (not checked).
- Whether Chrome's undisclosed Energy Saver "CPU-intensive" threshold catches a 100 µs spin-then-sleep loop that is idle most of the time.
