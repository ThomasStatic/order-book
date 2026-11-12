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

export class Page {
    constructor() {
        this.worker = new Worker(shimUrl, {workerData: {worker: workerUrl.href}});
        this.nextId = 1;
        this.replies = new Map();
        this.posted = [];   // every unsolicited message, in order
        this.waiters = [];
        this.failure = null;
        this.worker.on("message", (message) => this.receive(message));
        this.worker.on("error", (error) => this.fail(error));
    }

    receive(message) {
        if (typeof message.id === "number") {
            const waiting = this.replies.get(message.id);
            this.replies.delete(message.id);
            if (waiting) {
                waiting(message);
            }
            return;
        }
        this.posted.push(message);
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

    async init(store = []) {
        return this.request("init", {protocol: PROTOCOL_VERSION, ...moduleUrls(), store});
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

    async end() {
        await this.worker.terminate();
    }
}
