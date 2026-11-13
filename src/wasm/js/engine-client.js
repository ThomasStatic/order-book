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
        // The sandbox's command frames posted and not yet acknowledged by their answers, and
        // those waiting for the window.
        this.inFlight = 0;
        /** @type {Array<() => void>} */
        this.waitingForWindow = [];
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
        this.worker = new Worker(urls.worker, {type: "module"});
        this.worker.onmessage = (event) => this.receive(event.data);
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
    request(type, fields = {}) {
        const id = this.nextId++;
        return new Promise((resolve) => {
            this.replies.set(id, resolve);
            this.worker.postMessage({id, type, ...fields});
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
                break;
            }
            case "aborted":
                this.emit("aborted", message.message);
                break;
            default:
                // The page store's changes and seeded: kept by the page store, which comes next.
                break;
        }
    }

    async begin() {
        const urls = this.urls;
        const initialized = await this.request("init", {
            protocol: PROTOCOL_VERSION,
            module: String(urls.module),
            wasm: String(urls.wasm),
            store: [],
        });
        if (!initialized.ok) {
            this.worker.terminate();
            throw new Error("the engine worker refused to start: " + initialized.error);
        }
        if (initialized.protocol !== PROTOCOL_VERSION) {
            this.worker.terminate();
            throw new Error("the engine worker speaks protocol " + initialized.protocol + ", not " + PROTOCOL_VERSION);
        }
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
            this.setState("down");
            return {stdout: "", stderr: "start: " + reply.error + "\n", exit: 1};
        }
        const exit = await ready;
        const said = this.collecting || {report: "", diagnostics: ""};
        this.collecting = null;
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
        return {stdout: said.report, stderr: said.diagnostics, exit: exit.code};
    }

    /**
     * @param {string} line
     * @returns {Promise<RunResult>}
     */
    run(line) {
        const words = line.trim().split(/\s+/);
        if (words[0] === "start" || words[0] === "stop") {
            if (words.length > 1) {
                return Promise.resolve({stdout: "", stderr: words[0] + ": usage: " + words[0] + "\n", exit: 2});
            }
            // A second stop must not wait behind the first.
            return words[0] === "start" ? this.inTurn(() => this.start()) : this.stop();
        }
        return this.inTurn(async () => {
            const reply = await this.request("run", {line});
            if (!reply.ok) {
                return {stdout: "", stderr: reply.error + "\n", exit: 1};
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
        while (this.inFlight + COMMAND_FRAME_BYTES > CREDIT_WINDOW) {
            await new Promise((resolve) => this.waitingForWindow.push(() => resolve(undefined)));
        }
        this.inFlight += COMMAND_FRAME_BYTES;
        let reply;
        try {
            reply = await this.request(type, fields);
        } finally {
            this.inFlight -= COMMAND_FRAME_BYTES;
            const next = this.waitingForWindow.shift();
            if (next) {
                next();
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
        this.worker.postMessage({type: "consumed", bytes: reply.frameBytes});
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
