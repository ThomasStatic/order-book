// engine-client.js's crash path against the real engine worker and module under Node (spec #127,
// "The engine client"): the page store kept from the worker's changes, a crash that loses nothing
// answered, a fresh idle worker where verify and replay run and send can't connect, a start that
// recovers, everything waiting reported reset, an ended worker ignored, an abort and a forced
// stop taking the same path, a recovery that stops leaving the worker idle, and a reset. See
// docs/internals/testing.md and wasm.md.
import {test, after} from "node:test";
import assert from "node:assert/strict";
import {pathToFileURL} from "node:url";

import {installBrowser, endAll, workers} from "./browser.mjs";

installBrowser();
const client = await import(pathToFileURL(process.env.ORDER_BOOK_ENGINE_CLIENT).href);
const built = process.env.ORDER_BOOK_WASM_MODULE;
const urls = {
    worker: new URL("../../../src/wasm/js/engine-worker.js", import.meta.url),
    module: pathToFileURL(built),
    wasm: pathToFileURL(built.replace(/\.js$/, ".wasm")),
};

after(endAll);

function becomes(engine, state) {
    if (engine.state === state) {
        return Promise.resolve();
    }
    return new Promise((resolve) => {
        const off = engine.on("state", (now) => {
            if (now === state) {
                off();
                resolve();
            }
        });
    });
}

async function running() {
    const engine = await client.createEngine(urls);
    await becomes(engine, "running");
    return engine;
}

const current = () => workers[workers.length - 1];

test("a crash loses nothing answered, and a fresh worker verifies, replays, can't connect, and recovers", async () => {
    const engine = await running();
    const workersBefore = workers.length;
    for (const price of [95, 96, 97]) {
        await engine.submit("AAPL", {side: "BUY", quantity: 4, price, owner: 2});
    }
    const before = await engine.book("AAPL");

    await engine.crash();
    assert.equal(engine.state, "down");
    assert.equal(workers.length, workersBefore + 1, "a fresh worker");

    const verified = await engine.run("verify");
    assert.equal(verified.exit, 0, verified.stderr);
    assert.match(verified.stdout, /\nok\n$/);
    const replayed = await engine.run("replay valid/stops.csv --trades");
    assert.equal(replayed.exit, 0);
    assert.match(replayed.stdout, /^line 3: TradeExecuted/);
    assert.deepEqual(await engine.run("send valid/basic.csv --symbol AAPL"),
                     {stdout: "", stderr: "order_book_cli: cannot connect to 127.0.0.1:9000: Connection refused\n", exit: 1});
    await assert.rejects(engine.submit("AAPL", {side: "BUY", quantity: 1, price: 90}), /the engine isn't running/);

    const started = await engine.run("start");
    assert.equal(started.exit, 0);
    assert.match(started.stderr, /shard 0: recovered books=2/);
    assert.deepEqual(await engine.book("AAPL"), before, "every answered order is back");
    await engine.run("stop");
});

test("everything waiting is reported reset, and an ended worker's messages count for nothing", async () => {
    const engine = await running();
    const ended = current();
    const waiting = engine.submit("MSFT", {side: "BUY", quantity: 1, price: 150}).then(() => null, (error) => error);
    const line = engine.run("status");
    // Until the line has gone to the worker, on microtasks alone, so its reply, a message, can't
    // have been taken yet: the crash cuts it off.
    while (!ended.timeline.some((entry) => entry.sent && entry.sent.type === "run")) {
        await Promise.resolve();
    }
    await engine.crash();
    assert.match(String(await waiting), /crashed before it answered/);
    const cutOff = await line;
    assert.equal(cutOff.exit, 3, "a line the crash cut off exits 3");

    const heard = [];
    engine.on("line", (said) => heard.push(said));
    engine.on("state", (state) => heard.push(state));
    ended.onmessage({data: {type: "ready", instruments: [], placement: []}});
    ended.onmessage({data: {type: "line", stream: "report", text: "a ghost"}});
    ended.onmessage({data: {type: "remove", name: "log-0000-00000000000000000000.oblog"}});
    assert.equal(engine.state, "down");
    assert.deepEqual(heard, []);
    assert.equal((await engine.run("start")).exit, 0, "the store wasn't touched");
    await engine.run("stop");
});

test("an abort takes the crash path, and running out of memory points to reset", async () => {
    const engine = await running();
    await engine.submit("NVDA", {side: "SELL", quantity: 3, price: 55});
    const aborted = new Promise((resolve) => engine.on("aborted", resolve));
    current().onmessage({data: {type: "aborted", message: "OOM"}});
    assert.match(await aborted, /ran out of memory.*reset starts it again from the seed/);
    assert.equal(engine.state, "down");
    assert.equal((await engine.run("verify")).exit, 0, "a fresh worker holds the store");
    assert.equal((await engine.run("start")).exit, 0);
    const book = await engine.book("NVDA");
    assert.ok(book.asks.some((level) => level.price === 55), "and recovers it");
    await engine.run("stop");
});

test("a forced stop takes the crash path, so the next start has the threads it needs", async () => {
    const engine = await running();
    const workersBefore = workers.length;
    const first = engine.run("stop");
    await engine.run("stop");
    assert.equal((await first).exit, 8);
    const started = await engine.run("start");
    assert.equal(started.exit, 0, started.stderr);
    assert.equal(workers.length, workersBefore + 1, "on a fresh worker");
    await engine.run("stop");
});

test("a recovery that stops leaves the worker idle, with its message", async () => {
    const engine = await running();
    // Damage the page store's copy of shard 0's first log header, as a bad write would. (A bad
    // record is a torn tail, which recovery cuts, as natively.)
    const worker = current();
    const log = worker.timeline.map((entry) => entry.received)
        .find((message) => message && message.type === "rename" && /^log-0000-0+\.oblog$/.test(message.to));
    assert.ok(log, "shard 0 has a first log file");
    worker.onmessage({data: {type: "write", name: log.to, offset: 0, bytes: new Uint8Array(8).fill(0xA5)}});
    await engine.crash();

    const started = await engine.run("start");
    assert.equal(started.exit, 5, `recovery stops on the damaged header:\n${started.stderr}`);
    assert.match(started.stderr, /order_book_engine: /, "with its message");
    assert.equal(engine.state, "down");
    assert.notEqual((await engine.run("verify")).exit, 0, "verify sees it too");
    assert.equal((await engine.run("reset")).exit, 0, "and a reset starts again from the seed");
    assert.equal(engine.state, "running");
    await engine.run("stop");
});

test("end ends the worker and the store for good", async () => {
    const engine = await running();
    const worker = current();
    let terminated = false;
    const terminate = worker.terminate.bind(worker);
    worker.terminate = () => {
        terminated = true;
        return terminate();
    };
    const waiting = engine.submit("AAPL", {side: "BUY", quantity: 1, price: 90}).then(() => null, (error) => error);
    engine.end();
    assert.ok(terminated, "the worker is ended");
    assert.match(String(await waiting), /the engine has ended/);
    assert.equal(engine.state, "down");
    assert.deepEqual(await engine.run("status"), {stdout: "", stderr: "status: the engine has ended\n", exit: 1});
    const workersNow = workers.length;
    await engine.crash();
    assert.equal(workers.length, workersNow, "a crash after the end makes no new worker");
});

test("reset discards the store and starts again from the seed", async () => {
    const engine = await running();
    assert.equal((await engine.submit("TSLA", {side: "BUY", quantity: 1, price: 140})).orderId, 7);
    const result = await engine.run("reset");
    assert.equal(result.exit, 0);
    assert.equal(engine.state, "running");
    const book = await engine.book("TSLA");
    assert.deepEqual(book.bids.map((level) => level.price), [149, 148, 147], "only the seed");
    assert.equal((await engine.submit("TSLA", {side: "BUY", quantity: 1, price: 140})).orderId, 7, "IDs start again");
    await engine.run("stop");
});
