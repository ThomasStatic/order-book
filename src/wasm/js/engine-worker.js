// engine-worker.js: the engine worker (spec #127, "The module and the engine worker";
// docs/internals/wasm.md). Our own ES-module worker: it imports the module, whose pthread
// workers start from the module's own .js, relays messages between the page and the module,
// and services Emscripten's proxied calls by never blocking. The engine runs on a pthread of its
// own, started by `start`.
//
// Page to worker, each a request {id, type, ...} answered by {id, ok, ...}:
//   init {protocol, module, wasm, store}   first, and only once: nothing else is accepted before it
//   start                                   a run on the store; `ready` follows, or `exited`
//   stop                                    a first starts the clean stop, a second forces it
//   run {line}                              a terminal line: {stdout, stderr, exit} once answered
//   submit {symbol, order}, cancel {symbol, orderId}, replace {symbol, orderId, price, quantity}
//                                           the sandbox's: {orderId?, events, text, frameBytes} once
//                                           answered, or {ok: false, error}, its loss included
//   book {symbol}, status                   the shadows' book and each shard's records, at once
// and one notification, with no id and no reply:
//   consumed {connection, bytes}            the page has consumed this many bytes of the
//                                           "sandbox"'s answers or the "terminal"'s reading, which
//                                           that connection's credit window lets through again
// Worker to page, in one ordered channel with the replies:
//   create, write {offset, bytes}, extend {size}, cut {size}, rename {to}, replace {to}, remove,
//     each {name}: one page-store change each
//   line {stream: "report" | "diagnostics", text}
//   progress {connection: "terminal", bytes}   what the terminal read, for the page to acknowledge
//   seeded {stdout, stderr, exit}, ready {instruments, placement}, exited {code, name}
//   aborted {message}
// No JavaScript exception crosses postMessage: one escaping here, or the module aborting on any
// thread, is posted as `aborted`.

export const PROTOCOL_VERSION = 1;

let module = null;
let engine = null;
let initializing = false;
let pendingRun = null;  // the id of the run waiting for its answer
const pendingSandbox = new Map();  // the sandbox's correlation -> the id of the request waiting

function post(message, transfer = []) {
    self.postMessage(message, transfer);
}

function reply(id, fields) {
    post({id, ...fields});
}

// What a thrown value says: a C++ exception's type and what(), through the module's helper,
// or a JavaScript error's message.
function describe(what) {
    if (module && typeof WebAssembly.Exception === "function" && what instanceof WebAssembly.Exception) {
        try {
            const said = module.getExceptionMessage(what).filter(Boolean).join(": ");
            module.decrementExceptionRefcount(what);
            return said;
        } catch {
            // fall through to what the value itself says
        }
    }
    return String(what && what.message ? what.message : what);
}

function aborted(what) {
    post({type: "aborted", message: describe(what)});
}

// Posts what the module gave, in its order, the terminal's answer as the reply to its request.
function postEvents(events) {
    for (const event of events) {
        if (event.type === "answer") {
            if (pendingRun !== null) {
                const id = pendingRun;
                pendingRun = null;
                reply(id, {ok: true, stdout: event.stdout, stderr: event.stderr, exit: event.exit});
            }
        } else if (event.type === "sandboxAnswer") {
            const id = pendingSandbox.get(event.correlation);
            pendingSandbox.delete(event.correlation);
            if (id === undefined) {
                continue;
            }
            if (event.lost) {
                reply(id, {ok: false, error: event.lost});
            } else {
                const answer = {ok: true, events: event.events, text: event.text, frameBytes: event.frameBytes};
                if (event.orderId !== undefined) {
                    answer.orderId = event.orderId;
                }
                reply(id, answer);
            }
        } else if (event.type === "write") {
            post(event, [event.bytes.buffer]);
        } else {
            post(event);
        }
    }
}

// A sandbox command: refused at once, or waiting, under its correlation, for its answer, which
// may already be among the events.
function sandbox(id, result) {
    if (!result.ok) {
        reply(id, {ok: false, error: result.error});
    } else {
        pendingSandbox.set(result.correlation, id);
    }
    postEvents(result.events);
}

// Calls into the module under way on this thread. While this thread waits on a lock, Emscripten
// runs the calls queued for it, the module's request to look among them, so a look could start
// inside any call into the module, its embind wrapper included: inside a pump that a nested pump
// would end, or before `run` has made its request the one waiting for the answer. So a look
// asked for during a call waits for a task of its own.
let calling = 0;
let lookWanted = false;

function call(enter) {
    ++calling;
    try {
        return enter();
    } finally {
        if (--calling === 0 && lookWanted) {
            lookWanted = false;
            setTimeout(takeEvents, 0);
        }
    }
}

// The module's call, on this thread, whenever its queue has something.
function takeEvents() {
    if (!engine) {
        return;
    }
    if (calling !== 0) {
        lookWanted = true;
        return;
    }
    try {
        postEvents(call(() => engine.takeEvents()));
    } catch (error) {
        aborted(error);
    }
}

// Loads the module and makes the engine from the page store. A failure leaves nothing set, so
// init can be tried again.
async function init(request) {
    if (engine || initializing) {
        return {ok: false, error: "already initialized"};
    }
    if (request.protocol !== PROTOCOL_VERSION) {
        return {ok: false, error: "protocol", protocol: PROTOCOL_VERSION};
    }
    initializing = true;
    try {
        const {default: createOrderBookModule} = await import(request.module);
        const loaded = await createOrderBookModule({
            locateFile: (path) => (path.endsWith(".wasm") ? request.wasm : new URL(path, request.module).href),
            onAbort: aborted,
            // Nothing in the module prints, but if anything did it would be the engine's.
            print: (text) => post({type: "line", stream: "report", text}),
            printErr: (text) => post({type: "line", stream: "diagnostics", text}),
        });
        module = loaded;
        if (loaded.enterSamples() !== 0) {
            return {ok: false, error: "the module's samples are missing"};
        }
        const made = call(() => new loaded.Engine(request.store || []));
        loaded.onEngineEvents = takeEvents;
        engine = made;
        return {ok: true, protocol: PROTOCOL_VERSION, release: loaded.release()};
    } catch (error) {
        return {ok: false, error: describe(error)};
    } finally {
        initializing = false;
    }
}

async function handle(request) {
    if (request && request.type === "consumed" && request.id === undefined) {
        if (engine) {
            try {
                postEvents(call(() => engine.consumed(String(request.connection), Number(request.bytes) || 0)));
            } catch (error) {
                aborted(error);
            }
        }
        return;
    }
    if (!request || typeof request.id !== "number") {
        return;
    }
    const {id, type} = request;
    try {
        if (type === "init") {
            reply(id, await init(request));
            return;
        }
        if (!engine) {
            reply(id, {ok: false, error: "init comes first"});
            return;
        }
        if (type === "start") {
            const started = call(() => engine.start());
            reply(id, {ok: started.ok, ...(started.ok ? {} : {error: started.error})});
        } else if (type === "stop") {
            call(() => engine.stop());
            reply(id, {ok: true});
        } else if (type === "run") {
            if (pendingRun !== null) {
                reply(id, {ok: false, error: "the last line is still running"});
                return;
            }
            const result = call(() => engine.run(String(request.line)));
            if (!result.done) {
                pendingRun = id;  // its answer may be among the events
            }
            postEvents(result.events);
            if (result.done) {
                reply(id, {ok: true, stdout: result.stdout, stderr: result.stderr, exit: result.exit});
            }
        } else if (type === "submit") {
            sandbox(id, call(() => engine.submit(String(request.symbol), request.order || {})));
        } else if (type === "cancel") {
            sandbox(id, call(() => engine.cancel(String(request.symbol), request.orderId)));
        } else if (type === "replace") {
            sandbox(id, call(() => engine.replace(String(request.symbol), request.orderId, request.price, request.quantity)));
        } else if (type === "book") {
            const book = call(() => engine.book(String(request.symbol)));
            reply(id, book === null ? {ok: false, error: String(request.symbol) + " is not on the instrument list"} : {ok: true, book});
        } else if (type === "status") {
            reply(id, {ok: true, shards: call(() => engine.status())});
        } else {
            reply(id, {ok: false, error: "unknown request " + String(type)});
        }
    } catch (error) {
        // The request still gets its reply; the module may be broken, so the page hears that too.
        reply(id, {ok: false, error: describe(error)});
        aborted(error);
    }
}

// One request at a time, in the order they came, init's loading included.
let queue = Promise.resolve();
self.onmessage = (event) => {
    queue = queue.then(() => handle(event.data));
};
