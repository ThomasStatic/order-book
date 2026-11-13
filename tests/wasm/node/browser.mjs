// What engine-client.js expects of a browser, for the Node tests: a module Worker, here a Node
// worker thread running engine-worker.js through worker-shim.mjs, and cross-origin isolation.
// Every worker made records what went each way, so a test can check the page's credit window.
import {Worker as NodeWorker} from "node:worker_threads";

const shimUrl = new URL("./worker-shim.mjs", import.meta.url);

export const workers = [];

class ModuleWorker {
    constructor(url, options) {
        if (!options || options.type !== "module") {
            throw new Error("the engine worker is a module worker");
        }
        this.timeline = [];  // {sent} or {received}: every message each way, in the page's order
        this.onmessage = null;
        this.onerror = null;
        this.thread = new NodeWorker(shimUrl, {workerData: {worker: String(url)}});
        this.thread.on("message", (data) => {
            this.timeline.push({received: data});
            if (this.onmessage) {
                this.onmessage({data});
            }
        });
        this.thread.on("error", (error) => {
            if (this.onerror) {
                this.onerror(error);
            }
        });
        workers.push(this);
    }

    postMessage(message, transfer) {
        this.timeline.push({sent: message});
        this.thread.postMessage(message, transfer);
    }

    terminate() {
        return this.thread.terminate();
    }
}

export function installBrowser() {
    globalThis.Worker = ModuleWorker;
    globalThis.crossOriginIsolated = true;
}

export function endAll() {
    return Promise.all(workers.splice(0).map((worker) => worker.terminate()));
}
