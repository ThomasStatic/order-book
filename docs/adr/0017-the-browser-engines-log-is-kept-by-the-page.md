# The browser engine's log is kept by the page

In a browser tab a crash is the page ending the engine worker (ADR 0016), and anything inside that worker dies with it: the wasm heap, MEMFS, every pthread. So the engine's command logs and checkpoints are kept by the page, as the page store. The engine's own `LogStorage` holds its files in the wasm heap and reads them there, but every change it makes (a create, write, extend, cut, rename, or remove) goes through an injected callback before the call returns, and in the browser that callback is one waiting call to the engine worker, which posts the change to the page. A new engine worker is handed the page store before its start, and recovery runs over it unchanged.

A write is durable once the change is posted, which is what `process` durability means in a tab, and the only level offered there: with no drive under the store, `machine` would promise nothing more. The log's changes and the engine's answers reach the page over the same message channel, whose order is kept, so a `terminate()` that drops messages drops a tail of them, and the page never shows an answer whose log write it lacks. The page ignores every message from a worker it has ended, so a dying engine's late writes touch nothing.

## Considered options

- **MEMFS or WasmFS inside the module.** No code to write, but both live in the engine worker and die with it, and a second instance sees none of the first's files, which Emscripten 7.0.0 confirmed. A crash would lose the books, which is the opposite of what the page shows.
- **A SharedArrayBuffer the engine's threads write into directly.** No call to the engine worker per write, so faster. But it needs a file system laid out in shared memory, the buffer can reach each pthread only through an Emscripten internal (`PThread.unusedWorkers`), and since neither Emscripten nor the HTML spec stops nested workers before `terminate()` returns, the page would have to copy the store on every crash to fence off late writes. A tab's command rate doesn't need the speed.
- **OPFS.** It would outlive a reload too, but it is fog on the map: what `durable` would mean across a reload is undecided.

## Consequences

- The store is held twice, in the wasm heap and on the page, which counts against a tab's memory.
- Every log write costs a waiting call to the engine worker and a `postMessage`, so the engine worker's event loop must stay free, as ADR 0016 already requires.
- A reload or a closed tab loses every book, and the page says so.
- Natively the callback can simply record the changes, so the storage is tested without a browser.
- The directory lock is the page's one-engine rule; the storage's own lock is a flag, which still refuses `verify` while the engine runs.
