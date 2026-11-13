// Runs engine-worker.js in a Node worker thread as a browser runs it in a module worker: `self`
// is the global, with postMessage and onmessage over the thread's port. The worker's URL comes in
// workerData, which isn't Emscripten's "em-pthread", so the module knows it is no pthread.
import {parentPort, workerData} from "node:worker_threads";

globalThis.self = globalThis;
globalThis.postMessage = (message, transfer) => parentPort.postMessage(message, transfer);
parentPort.on("message", (data) => {
    if (typeof globalThis.onmessage === "function") {
        globalThis.onmessage({data});
    }
});

await import(workerData.worker);
