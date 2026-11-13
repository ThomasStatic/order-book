// The page, as the Node tests play it: the real engine-worker.js in a worker thread, given the
// module built by CMake (ORDER_BOOK_WASM_MODULE, the path of order_book_wasm.js), and the
// session transcript that tests/wasm_terminal_test.cpp prints too. See docs/internals/wasm.md
// and testing.md.
import {Worker} from "node:worker_threads";
import {pathToFileURL} from "node:url";

const workerUrl = new URL("../../../src/wasm/js/engine-worker.js", import.meta.url);
const shimUrl = new URL("./worker-shim.mjs", import.meta.url);

export const PROTOCOL_VERSION = 1;

export function moduleUrls() {
    const built = process.env.ORDER_BOOK_WASM_MODULE;
    if (!built) {
        throw new Error("ORDER_BOOK_WASM_MODULE names the built order_book_wasm.js");
    }
    return {module: pathToFileURL(built).href, wasm: pathToFileURL(built.replace(/\.js$/, ".wasm")).href};
}

// A text's lines as std::getline gives them: no empty last line for a final newline.
export function linesOf(text) {
    const lines = text.split("\n");
    if (lines.length > 0 && lines[lines.length - 1] === "") {
        lines.pop();
    }
    return lines;
}

// The page store: every file the worker's changes made, as the page keeps them, so a fresh
// worker can be given them after a crash.
export class PageStore {
    constructor(files = []) {
        this.files = new Map(files.map((file) => [file.name, file.bytes.slice()]));
    }

    // One posted change; false for a message that isn't one.
    apply(message) {
        const {type, name} = message;
        const grown = (bytes, size) => {
            const next = new Uint8Array(size);
            next.set(bytes.subarray(0, Math.min(bytes.length, size)));
            return next;
        };
        if (type === "create") {
            this.files.set(name, new Uint8Array(0));
        } else if (type === "write") {
            let bytes = this.files.get(name);
            const end = message.offset + message.bytes.length;
            if (bytes.length < end) {
                bytes = grown(bytes, end);
            }
            bytes.set(message.bytes, message.offset);
            this.files.set(name, bytes);
        } else if (type === "extend" || type === "cut") {
            this.files.set(name, grown(this.files.get(name), message.size));
        } else if (type === "rename" || type === "replace") {
            this.files.set(message.to, this.files.get(name));
            this.files.delete(name);
        } else if (type === "remove") {
            this.files.delete(name);
        } else {
            return false;
        }
        return true;
    }

    names() {
        return [...this.files.keys()].sort();
    }

    // As init takes them: each a copy, so the page keeps its own.
    snapshot() {
        return this.names().map((name) => ({name, bytes: this.files.get(name).slice()}));
    }
}

export class Page {
    // A new worker, given this store's files at init.
    constructor(store = new PageStore()) {
        this.worker = new Worker(shimUrl, {workerData: {worker: workerUrl.href}});
        this.store = store;
        this.nextId = 1;
        this.replies = new Map();
        this.posted = [];   // every unsolicited message, in order
        this.waiters = [];
        this.failure = null;
        this.ended = false;
        this.worker.on("message", (message) => this.receive(message));
        this.worker.on("error", (error) => this.fail(error));
    }

    receive(message) {
        if (this.ended) {
            return;  // a worker the page has ended touches nothing
        }
        if (typeof message.id === "number") {
            const waiting = this.replies.get(message.id);
            this.replies.delete(message.id);
            if (waiting) {
                waiting(message);
            }
            return;
        }
        this.store.apply(message);
        if (message.type === "progress") {
            // Acknowledged at once, as the engine client does, so the terminal's window stays open.
            this.worker.postMessage({type: "consumed", connection: message.connection, bytes: message.bytes});
        }
        // A write's bytes live in the store; the log of what came keeps where they went.
        this.posted.push(message.type === "write" ? {type: "write", name: message.name, offset: message.offset, length: message.bytes.length} : message);
        if (message.type === "aborted") {
            this.fail(new Error("the module aborted: " + message.message));
        }
        this.waiters = this.waiters.filter((waiter) => !waiter(message));
    }

    fail(error) {
        this.failure = error;
        for (const waiter of this.waiters) {
            waiter(null, error);
        }
        this.waiters = [];
        for (const [, waiting] of this.replies) {
            waiting({ok: false, error: String(error)});
        }
        this.replies.clear();
    }

    request(type, fields = {}) {
        const id = this.nextId++;
        return new Promise((resolve) => {
            this.replies.set(id, resolve);
            this.worker.postMessage({id, type, ...fields});
        });
    }

    // The next posted message of this type from now on.
    next(type) {
        return new Promise((resolve, reject) => {
            if (this.failure) {
                reject(this.failure);
                return;
            }
            this.waiters.push((message, error) => {
                if (error) {
                    reject(error);
                    return true;
                }
                if (message.type === type) {
                    resolve(message);
                    return true;
                }
                return false;
            });
        });
    }

    async init() {
        return this.request("init", {protocol: PROTOCOL_VERSION, ...moduleUrls(), store: this.store.snapshot()});
    }

    // Resolves once the predicate holds, checked now and after every posted message.
    until(predicate) {
        return new Promise((resolve, reject) => {
            if (predicate()) {
                resolve();
                return;
            }
            if (this.failure) {
                reject(this.failure);
                return;
            }
            this.waiters.push((message, error) => {
                if (error) {
                    reject(error);
                    return true;
                }
                if (predicate()) {
                    resolve();
                    return true;
                }
                return false;
            });
        });
    }

    // A start, to its ready.
    async start() {
        const ready = this.next("ready");
        const started = await this.request("start");
        if (!started.ok) {
            throw new Error("start refused: " + started.error);
        }
        return ready;
    }

    async stop() {
        const exited = this.next("exited");
        await this.request("stop");
        return exited;
    }

    // Each line typed, after "> ", then what it printed, its stderr lines after "! ", and
    // "[exit N]" when not 0: the transcript tests/wasm/sessions/<name>.expected holds.
    async play(lines) {
        let transcript = "";
        for (const line of lines) {
            transcript += `> ${line}\n`;
            const result = await this.request("run", {line});
            if (!result.ok) {
                throw new Error(`run ${line}: ${result.error}`);
            }
            transcript += result.stdout;
            for (const error of linesOf(result.stderr)) {
                transcript += `! ${error}\n`;
            }
            if (result.exit !== 0) {
                transcript += `[exit ${result.exit}]\n`;
            }
        }
        return transcript;
    }

    // Ends the worker without warning, as a crash does: what it posted after this is ignored.
    async end() {
        this.ended = true;
        await this.worker.terminate();
    }
}
