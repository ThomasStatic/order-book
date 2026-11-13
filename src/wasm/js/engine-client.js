// @ts-check
// engine-client.js: the engine client (spec #127, "The engine client"; docs/internals/wasm.md).
// The only thing a page imports, typed by the hand-written engine-client.d.ts. It holds the
// message protocol with the engine worker and the page's side of the credit windows, and runs
// start and stop itself, since they make or end what runs.

/** @typedef {import("./engine-client").Support} Support */
/** @typedef {import("./engine-client").MissingSupport} MissingSupport */
/** @typedef {import("./engine-client").EngineUrls} EngineUrls */
/** @typedef {import("./engine-client").Engine} Engine */
/** @typedef {import("./engine-client").EngineState} EngineState */
/** @typedef {import("./engine-client").EngineEvents} EngineEvents */
/** @typedef {import("./engine-client").RunResult} RunResult */
/** @typedef {import("./engine-client").OrderInput} OrderInput */
/** @typedef {import("./engine-client").Answer} Answer */
/** @typedef {import("./engine-client").Book} Book */
/** @typedef {import("./engine-client").ShardStatus} ShardStatus */

// CMake writes the release in when it copies this file beside the module.
export const ENGINE_RELEASE = "__ORDER_BOOK_ENGINE_RELEASE__";
export const PROTOCOL_VERSION = 1;

// W, settings.hpp's creditWindow: the frame bytes in flight each way per connection.
const CREDIT_WINDOW = 64 * 1024;
// Every command frame's size (docs/wire-protocol.md), which the page's window counts.
const COMMAND_FRAME_BYTES = 48;

// A module with a shared memory, which only a browser with wasm threads validates.
const THREADS_PROBE = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 5, 4, 1, 3, 1, 1]);
// A function with a legacy try and catch_all, as -fwasm-exceptions emits.
const EXCEPTIONS_PROBE = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 8, 1, 6, 0, 6, 64, 25, 11, 11,
]);

export class EngineSupportError extends Error {
    /** @param {MissingSupport} missing */
    constructor(missing) {
        super("the engine can't run here: " + missing + " is missing");
        this.name = "EngineSupportError";
        /** @readonly */
        this.missing = missing;
    }
}

/** @returns {Support} */
export function checkSupport() {
    if (globalThis.crossOriginIsolated !== true) {
        return {ok: false, missing: "isolation"};
    }
    try {
        const channel = new MessageChannel();
        channel.port1.postMessage(new SharedArrayBuffer(8));
        channel.port1.close();
        channel.port2.close();
    } catch {
        return {ok: false, missing: "shared-memory"};
    }
    if (!WebAssembly.validate(THREADS_PROBE)) {
        return {ok: false, missing: "threads"};
    }
    if (!WebAssembly.validate(EXCEPTIONS_PROBE)) {
        return {ok: false, missing: "exceptions"};
    }
    return {ok: true};
}

/**
 * @param {EngineUrls} urls
 * @returns {Promise<Engine>}
 */
export async function createEngine(urls) {
    const support = checkSupport();
    if (!support.ok) {
        throw new EngineSupportError(support.missing);
    }
    const engine = new BrowserEngine(urls);
    await engine.begin();
    return engine;
}

// An abort that says memory ran out: recovery would run out again, so only a reset helps.
const OUT_OF_MEMORY = /\bOOM\b|out of memory|Cannot enlarge memory/i;

/**
 * The page store: every file the engine's log and checkpoints are, kept from the worker's change
 * messages, so a crash loses nothing answered, and each new worker is given a copy.
 */
class PageStore {
    constructor() {
        /** @type {Map<string, Uint8Array>} */
        this.files = new Map();
    }

    /**
     * One change message; false for a message that isn't one.
     * @param {any} change
     */
    apply(change) {
        const name = String(change.name);
        /** @param {Uint8Array | undefined} bytes @param {number} size */
        const sized = (bytes, size) => {
            const next = new Uint8Array(size);
            if (bytes) {
                next.set(bytes.subarray(0, Math.min(bytes.length, size)));
            }
            return next;
        };
        switch (change.type) {
            case "create":
                this.files.set(name, new Uint8Array(0));
                return true;
            case "write": {
                /** @type {Uint8Array} */
                const written = change.bytes;
                let bytes = this.files.get(name) || new Uint8Array(0);
                const end = change.offset + written.length;
                if (bytes.length < end) {
                    bytes = sized(bytes, end);
                }
                bytes.set(written, change.offset);
                this.files.set(name, bytes);
                return true;
            }
            case "extend":
            case "cut":
                this.files.set(name, sized(this.files.get(name), change.size));
                return true;
            case "rename":
            case "replace": {
                const bytes = this.files.get(name);
                this.files.delete(name);
                this.files.set(String(change.to), bytes || new Uint8Array(0));
                return true;
            }
            case "remove":
                this.files.delete(name);
                return true;
            default:
                return false;
        }
    }

    /** What init takes; postMessage clones it, so the page keeps its own. */
    snapshot() {
        return [...this.files].map(([name, bytes]) => ({name, bytes}));
    }
}

/** @param {string} text */
function linesOf(text) {
    return text === "" ? "" : text + "\n";
}

/** @implements {Engine} */
class BrowserEngine {
    /** @param {EngineUrls} urls */
    constructor(urls) {
        this.urls = urls;
        /** @type {EngineState} */
        this.current = "down";
        /** @type {Map<number, (reply: any) => void>} */
        this.replies = new Map();
        this.nextId = 1;
        /** @type {{[K in keyof EngineEvents]: Set<(value: EngineEvents[K]) => void>}} */
        this.listeners = {state: new Set(), line: new Set(), aborted: new Set(), exited: new Set()};
        // The sandbox's command frames posted and not yet acknowledged by their answers, and the
        // commands waiting for the window, first come first served: a freed slot passes straight
        // to the first of them, so commands go in the order they were made.
        this.inFlight = 0;
        /** @type {Array<() => void>} */
        this.waitingForWindow = [];
        // One crash path at a time, in order.
        /** @type {Promise<void>} */
        this.ending = Promise.resolve();
        // Lines one at a time, in order, as the terminal types them.
        /** @type {Promise<unknown>} */
        this.lines = Promise.resolve();
        // What the current start or stop collects of the engine's lines.
        /** @type {{report: string, diagnostics: string} | null} */
        this.collecting = null;
        /** @type {((exit: import("./engine-client").EngineExit | null) => void) | null} */
        this.started = null;
        /** @type {((exit: import("./engine-client").EngineExit) => void) | null} */
        this.stopped = null;
        this.store = new PageStore();
        /** @type {Worker | null} */
        this.worker = null;
        // A new worker being made and given the store, which requests wait for.
        /** @type {Promise<void> | null} */
        this.opening = null;
    }

    // A new worker, given a copy of the page store, idle until a start. A message from any worker
    // but the current one is ignored, so an ended worker's late messages touch nothing.
    async open() {
        const worker = new Worker(this.urls.worker, {type: "module"});
        this.worker = worker;
        worker.onmessage = (event) => {
            if (worker === this.worker) {
                this.receive(event.data);
            }
        };
        const opened = (async () => {
            const initialized = await this.post(worker, "init", {
                protocol: PROTOCOL_VERSION,
                module: String(this.urls.module),
                wasm: String(this.urls.wasm),
                store: this.store.snapshot(),
            });
            if (!initialized.ok || initialized.protocol !== PROTOCOL_VERSION) {
                if (this.worker === worker) {
                    this.worker = null;
                }
                worker.terminate();
                throw new Error(initialized.ok ? "the engine worker speaks protocol " + initialized.protocol + ", not " + PROTOCOL_VERSION
                                               : "the engine worker refused to start: " + initialized.error);
            }
        })();
        // Requests wait on it; a failure is opened's to report, so this copy never rejects.
        const settled = opened.then(
            () => undefined,
            () => undefined,
        );
        this.opening = settled;
        settled.then(() => {
            if (this.opening === settled) {
                this.opening = null;
            }
        });
        await opened;
    }

    /**
     * Ends the worker without warning, as a crash does, then makes a fresh idle one holding the
     * page store. Everything waiting on the old one is reported reset with why.
     * @param {string} why
     */
    endWorker(why) {
        const next = this.ending.then(() => this.replaceWorker(why));
        this.ending = next.catch(() => undefined);
        return next;
    }

    /** @param {string} why */
    async replaceWorker(why) {
        const ended = this.worker;
        this.worker = null;
        if (ended) {
            ended.terminate();
        }
        for (const [, waiting] of this.replies) {
            waiting({ok: false, error: why, reset: true});
        }
        this.replies.clear();
        const crashed = {code: 3, name: "CRASHED"};
        if (this.started) {
            this.started(crashed);
            this.started = null;
        }
        if (this.stopped) {
            this.stopped(crashed);
            this.stopped = null;
        }
        this.setState("down");
        try {
            await this.open();
        } catch (error) {
            this.emit("aborted", "the engine worker couldn't start again: " + String(error instanceof Error ? error.message : error));
            throw error;
        }
    }

    /** @returns {Promise<void>} */
    async crash() {
        await this.endWorker("the engine crashed before it answered: what ran is unknown");
    }

    /** @returns {Promise<void>} */
    async reset() {
        this.store = new PageStore();
        await this.endWorker("the engine was reset before it answered: what ran is unknown");
        await this.start();
    }

    get state() {
        return this.current;
    }

    /** @param {EngineState} state */
    setState(state) {
        if (this.current !== state) {
            this.current = state;
            this.emit("state", state);
        }
    }

    /**
     * @template {keyof EngineEvents} K
     * @param {K} event
     * @param {EngineEvents[K]} value
     */
    emit(event, value) {
        for (const listener of this.listeners[event]) {
            listener(value);
        }
    }

    /**
     * @template {keyof EngineEvents} K
     * @param {K} event
     * @param {(value: EngineEvents[K]) => void} listener
     * @returns {() => void}
     */
    on(event, listener) {
        this.listeners[event].add(listener);
        return () => {
            this.listeners[event].delete(listener);
        };
    }

    /**
     * @param {string} type
     * @param {Record<string, unknown>} [fields]
     * @returns {Promise<any>}
     */
    async request(type, fields = {}) {
        if (this.opening) {
            await this.opening;
        }
        if (!this.worker) {
            return {ok: false, error: "the engine worker has ended"};
        }
        return this.post(this.worker, type, fields);
    }

    /**
     * @param {Worker} worker
     * @param {string} type
     * @param {Record<string, unknown>} fields
     * @returns {Promise<any>}
     */
    post(worker, type, fields) {
        const id = this.nextId++;
        return new Promise((resolve) => {
            this.replies.set(id, resolve);
            worker.postMessage({id, type, ...fields});
        });
    }

    /** @param {any} message */
    receive(message) {
        if (typeof message.id === "number") {
            const waiting = this.replies.get(message.id);
            this.replies.delete(message.id);
            if (waiting) {
                waiting(message);
            }
            return;
        }
        switch (message.type) {
            case "line":
                if (this.collecting) {
                    this.collecting[message.stream === "report" ? "report" : "diagnostics"] += linesOf(message.text);
                }
                this.emit("line", {stream: message.stream, text: message.text});
                break;
            case "ready":
                this.setState("running");
                if (this.started) {
                    this.started(null);
                    this.started = null;
                }
                break;
            case "exited": {
                const exit = {code: message.code, name: message.name};
                this.setState("down");
                if (this.started) {
                    this.started(exit);
                    this.started = null;
                }
                if (this.stopped) {
                    this.stopped(exit);
                    this.stopped = null;
                }
                this.emit("exited", exit);
                // A forced stop leaves the engine's threads behind, which a module can't start
                // again: the crash path, with nothing waiting.
                if (exit.code === 8) {
                    this.endWorker("the engine was forced to stop before it answered: what ran is unknown").catch(() => undefined);
                }
                break;
            }
            case "progress":
                if (this.worker) {
                    this.worker.postMessage({type: "consumed", connection: message.connection, bytes: message.bytes});
                }
                break;
            case "aborted": {
                const said = String(message.message);
                const why = OUT_OF_MEMORY.test(said)
                    ? "the engine ran out of memory, and recovery would too: reset starts it again from the seed (" + said + ")"
                    : "the engine aborted: " + said;
                this.emit("aborted", why);
                this.endWorker(why).catch(() => undefined);
                break;
            }
            default:
                // The page store's changes; seeded needs nothing.
                this.store.apply(message);
                break;
        }
    }

    async begin() {
        await this.open();
        void this.start();
    }

    /** @returns {Promise<RunResult>} */
    async start() {
        if (this.current !== "down") {
            return {stdout: "", stderr: "start: the engine is already " + this.current + "\n", exit: 2};
        }
        this.setState("starting");
        this.collecting = {report: "", diagnostics: ""};
        /** @type {Promise<import("./engine-client").EngineExit | null>} */
        const ready = new Promise((resolve) => {
            this.started = resolve;
        });
        const reply = await this.request("start");
        if (!reply.ok) {
            this.started = null;
            this.collecting = null;
            if (!reply.reset) {
                this.setState("down");
            }
            return {stdout: "", stderr: "start: " + reply.error + "\n", exit: reply.reset ? 3 : 1};
        }
        const exit = await ready;
        const said = this.collecting || {report: "", diagnostics: ""};
        this.collecting = null;
        if (exit && exit.name === "CRASHED") {
            return {stdout: said.report, stderr: said.diagnostics + "start: the engine crashed before it was ready\n", exit: 3};
        }
        return {stdout: said.report, stderr: said.diagnostics, exit: exit ? exit.code : 0};
    }

    /** @returns {Promise<RunResult>} */
    async stop() {
        if (this.current === "down") {
            return {stdout: "", stderr: "stop: the engine isn't running\n", exit: 1};
        }
        if (this.current === "stopping") {
            // A second stop forces the first, whose line reports how it ended.
            await this.request("stop");
            return {stdout: "", stderr: "", exit: 0};
        }
        this.setState("stopping");
        this.collecting = {report: "", diagnostics: ""};
        /** @type {Promise<import("./engine-client").EngineExit>} */
        const exited = new Promise((resolve) => {
            this.stopped = resolve;
        });
        await this.request("stop");
        const exit = await exited;
        const said = this.collecting || {report: "", diagnostics: ""};
        this.collecting = null;
        if (exit.name === "CRASHED") {
            return {stdout: said.report, stderr: said.diagnostics + "stop: the engine crashed before it stopped\n", exit: 3};
        }
        return {stdout: said.report, stderr: said.diagnostics, exit: exit.code};
    }

    /**
     * @param {string} line
     * @returns {Promise<RunResult>}
     */
    run(line) {
        const words = line.trim().split(/\s+/);
        const lifecycle = ["start", "stop", "crash", "reset"];
        if (lifecycle.includes(words[0])) {
            const verb = words[0];
            if (words.length > 1) {
                return Promise.resolve({stdout: "", stderr: verb + ": usage: " + verb + "\n", exit: 2});
            }
            if (verb === "start") {
                return this.inTurn(() => this.start());
            }
            if (verb === "stop") {
                return this.stop();  // a second stop must not wait behind the first
            }
            // A crash or a reset doesn't wait behind a line either: it ends what that line waits on.
            const done = verb === "crash" ? this.crash() : this.reset();
            return done.then(() => ({stdout: "", stderr: "", exit: 0}));
        }
        return this.inTurn(async () => {
            const reply = await this.request("run", {line});
            if (!reply.ok) {
                return {stdout: "", stderr: words[0] + ": " + reply.error + "\n", exit: reply.reset ? 3 : 1};
            }
            return {stdout: reply.stdout, stderr: reply.stderr, exit: reply.exit};
        });
    }

    /**
     * @template T
     * @param {() => Promise<T>} work
     * @returns {Promise<T>}
     */
    inTurn(work) {
        const turn = this.lines.then(work, work);
        this.lines = turn.catch(() => undefined);
        return turn;
    }

    /**
     * A sandbox command, posted once the page's window has room for its frame; its answer
     * acknowledges the frame, and the page acknowledges the answer's frames once it has them.
     * @param {string} type
     * @param {Record<string, unknown>} fields
     * @returns {Promise<Answer>}
     */
    async command(type, fields) {
        if (this.current !== "running") {
            throw new Error("the engine isn't running");
        }
        if (this.waitingForWindow.length > 0 || this.inFlight + COMMAND_FRAME_BYTES > CREDIT_WINDOW) {
            // Waits its turn; the slot it is woken with is already counted.
            await new Promise((resolve) => this.waitingForWindow.push(() => resolve(undefined)));
        } else {
            this.inFlight += COMMAND_FRAME_BYTES;
        }
        const worker = this.worker;
        let reply;
        try {
            reply = this.current === "running" ? await this.request(type, fields) : {ok: false, error: "the engine isn't running"};
        } finally {
            const next = this.waitingForWindow.shift();
            if (next) {
                next();  // the slot passes on
            } else {
                this.inFlight -= COMMAND_FRAME_BYTES;
            }
        }
        if (!reply.ok) {
            throw new Error(reply.error);
        }
        /** @type {Answer} */
        const answer = {events: reply.events, text: reply.text};
        if (reply.orderId !== undefined) {
            answer.orderId = reply.orderId;
        }
        // To the worker that answered: a crash since has given the page a fresh one.
        if (worker && worker === this.worker) {
            worker.postMessage({type: "consumed", connection: "sandbox", bytes: reply.frameBytes});
        }
        return answer;
    }

    /**
     * @param {string} symbol
     * @param {OrderInput} order
     */
    submit(symbol, order) {
        return this.command("submit", {symbol, order});
    }

    /**
     * @param {string} symbol
     * @param {number} orderId
     */
    cancel(symbol, orderId) {
        return this.command("cancel", {symbol, orderId});
    }

    /**
     * @param {string} symbol
     * @param {number} orderId
     * @param {number} price
     * @param {number} quantity
     */
    replace(symbol, orderId, price, quantity) {
        return this.command("replace", {symbol, orderId, price, quantity});
    }

    /**
     * @param {string} symbol
     * @returns {Promise<Book>}
     */
    async book(symbol) {
        const reply = await this.request("book", {symbol});
        if (!reply.ok) {
            throw new Error(reply.error);
        }
        return reply.book;
    }

    /** @returns {Promise<ShardStatus[]>} */
    async status() {
        const reply = await this.request("status");
        if (!reply.ok) {
            throw new Error(reply.error);
        }
        return reply.shards;
    }
}

// The exports, held to their declarations in engine-client.d.ts.
/** @type {{
 *   ENGINE_RELEASE: typeof import("./engine-client").ENGINE_RELEASE,
 *   PROTOCOL_VERSION: typeof import("./engine-client").PROTOCOL_VERSION,
 *   checkSupport: typeof import("./engine-client").checkSupport,
 *   createEngine: typeof import("./engine-client").createEngine,
 *   EngineSupportError: typeof import("./engine-client").EngineSupportError,
 * }} */
const declared = {ENGINE_RELEASE, PROTOCOL_VERSION, checkSupport, createEngine, EngineSupportError};
void declared;
