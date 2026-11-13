// engine-client.js against the real engine worker and module under Node (spec #127, "Testing
// Decisions" 5): support and createEngine, the terminal through run, start and stop through run,
// the sandbox's typed calls and their decoded answers, both standing connections reopening after
// a start, the page's credit window, and a page that stops consuming pushing back. See
// docs/internals/testing.md and wasm.md.
import {test, after} from "node:test";
import assert from "node:assert/strict";
import {pathToFileURL} from "node:url";

import {installBrowser, endAll, workers} from "./browser.mjs";
import {Page} from "./page.mjs";

installBrowser();
const client = await import(pathToFileURL(process.env.ORDER_BOOK_ENGINE_CLIENT).href);
const built = process.env.ORDER_BOOK_WASM_MODULE;
const urls = {
    worker: new URL("../../../src/wasm/js/engine-worker.js", import.meta.url),
    module: pathToFileURL(built),
    wasm: pathToFileURL(built.replace(/\.js$/, ".wasm")),
};

after(endAll);

// Resolves at the next state change to this one, or now.
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
    assert.equal(engine.state, "starting");
    await becomes(engine, "running");
    return engine;
}

test("support is checked first, and createEngine rejects with what is missing", async () => {
    assert.deepEqual(client.checkSupport(), {ok: true});
    assert.equal(client.PROTOCOL_VERSION, 1);
    assert.match(client.ENGINE_RELEASE, /^v\d+\.\d+\.\d+$/);
    globalThis.crossOriginIsolated = false;
    try {
        assert.deepEqual(client.checkSupport(), {ok: false, missing: "isolation"});
        await assert.rejects(client.createEngine(urls), (error) => error instanceof client.EngineSupportError && error.missing === "isolation");
    } finally {
        globalThis.crossOriginIsolated = true;
    }
});

test("the client refuses a worker that speaks another protocol", async () => {
    const fake = {...urls, worker: new URL("./fake-worker.mjs", import.meta.url)};
    await assert.rejects(client.createEngine(fake), /speaks protocol 2, not 1/);
});

test("the terminal, the sandbox, and start and stop through run", async () => {
    const engine = await running();
    const lines = [];
    engine.on("line", (line) => lines.push(line));

    // The terminal, as the native CLI prints it.
    assert.deepEqual(await engine.run("buy AAPL 1 90"),
                     {stdout: "assigned id=7\nline 2: OrderAccepted id=7 side=BUY price=90 qty=1\n", stderr: "", exit: 0});

    // The sandbox, on its own connection, after the terminal's ID.
    const bid = await engine.submit("AAPL", {side: "BUY", quantity: 5, price: 100});
    assert.equal(bid.orderId, 8);
    assert.deepEqual(bid.events, [{type: "OrderAccepted", orderId: 8, side: "BUY", price: 100, quantity: 5,
                                   text: "OrderAccepted id=8 side=BUY price=100 qty=5"}]);
    assert.equal(bid.text, "line 1: OrderAccepted id=8 side=BUY price=100 qty=5\n");
    const market = await engine.submit("AAPL", {side: "SELL", quantity: 7});
    assert.deepEqual(market.events.map((event) => event.type), ["OrderAccepted", "TradeExecuted", "TradeExecuted"]);
    assert.equal(market.events[1].restingOrderId, 8);
    assert.deepEqual((await engine.cancel("AAPL", 999)).events.map((event) => [event.type, event.reason]),
                     [["CancelRejected", "ORDER_NOT_FOUND"]]);
    assert.equal((await engine.replace("AAPL", 4, 104, 3)).events[0].type, "OrderReplaced");
    await assert.rejects(engine.submit("XYZ", {side: "BUY", quantity: 1, price: 1}), /XYZ is not on the instrument list/);
    await assert.rejects(engine.submit("AAPL", {side: "BUY", quantity: 0, price: 1}), /quantity must be positive/);

    const stop = await engine.submit("NVDA", {side: "BUY", quantity: 2, price: 60, stopPrice: 55, owner: 3});
    const book = await engine.book("NVDA");
    assert.deepEqual(book.stops, [{orderId: stop.orderId, side: "BUY", type: "STOP_LIMIT", stopPrice: 55, price: 60, quantity: 2,
                                   timeInForce: "GTC", owner: 3, selfTradePrevention: "CANCEL_RESTING", postOnly: false}]);
    assert.deepEqual(book.bids[0], {price: 49, quantity: 10, orders: [{orderId: 1, quantity: 10, initialQuantity: 10}]});
    assert.equal(book.lastTradePrice, null);
    await assert.rejects(engine.book("XYZ"), /not on the instrument list/);
    const status = await engine.status();
    assert.deepEqual(status.map((shard) => shard.instruments), [["AAPL", "MSFT"], ["NVDA", "TSLA"]]);

    // One engine at a time; the first stop is clean, with its report.
    assert.deepEqual(await engine.run("start"), {stdout: "", stderr: "start: the engine is already running\n", exit: 2});
    const exited = new Promise((resolve) => engine.on("exited", resolve));
    const stopped = await engine.run("stop");
    assert.equal(stopped.exit, 0);
    assert.notEqual(stopped.stdout, "", "the stop report");
    assert.deepEqual(await exited, {code: 0, name: "CLEAN_STOP"});
    assert.equal(engine.state, "down");
    assert.deepEqual(await engine.run("stop"), {stdout: "", stderr: "stop: the engine isn't running\n", exit: 1});
    await assert.rejects(engine.submit("AAPL", {side: "BUY", quantity: 1, price: 90}), /the engine isn't running/);
    assert.deepEqual(await engine.run("buy AAPL 1 90"), {stdout: "", stderr: "buy: the engine isn't running\n", exit: 1});

    // A start after a clean stop recovers, and both standing connections reopen.
    const started = await engine.run("start");
    assert.equal(started.exit, 0);
    assert.match(started.stderr, /shard 0: recovered books=2/);
    assert.equal(engine.state, "running");
    const again = await engine.submit("AAPL", {side: "BUY", quantity: 1, price: 95});
    assert.ok(again.orderId > 8, "IDs go on past what the store holds");
    const terminal = await engine.run("buy MSFT 1 190");
    assert.equal(terminal.exit, 0);
    assert.match(terminal.stdout, /^assigned id=\d+\nline \d+: OrderAccepted/);
    assert.ok(lines.some((line) => line.stream === "report"), "lines are heard as they come");

    // A second stop forces the first.
    const first = engine.run("stop");
    const second = await engine.run("stop");
    assert.deepEqual(second, {stdout: "", stderr: "", exit: 0});
    assert.equal((await first).exit, 8, "the first reports the forced stop");
});

test("the page's window: never more than W of command frames in flight", async () => {
    const engine = await running();
    const worker = workers[workers.length - 1];
    // 2,000 commands of 48 bytes each are more than W = 64 KiB, so some wait.
    const answers = await Promise.all(Array.from({length: 2000}, (_, i) => engine.submit("TSLA", {side: "SELL", quantity: 1, price: 200 + i})));
    assert.deepEqual(answers.map((answer) => answer.events[0].type), Array(2000).fill("OrderAccepted"));
    assert.deepEqual(answers.map((answer) => answer.orderId), answers.map((_, i) => 7 + i), "in order");

    // What went each way, in the page's order: each command's 48-byte frame is in flight from
    // its post until its answer.
    const commands = new Set();
    let inFlight = 0;
    let most = 0;
    for (const {sent, received} of worker.timeline) {
        if (sent && ["submit", "cancel", "replace"].includes(sent.type)) {
            commands.add(sent.id);
            inFlight += 48;
            most = Math.max(most, inFlight);
        } else if (received && commands.has(received.id)) {
            inFlight -= 48;
        }
    }
    assert.equal(commands.size, 2000);
    assert.ok(most <= 65536, `at most W in flight, saw ${most}`);
    assert.ok(most > 65536 - 48 * 10, `and the window filled, at ${most}`);
    await engine.run("stop");
});

test("a page that stops consuming pushes back: the worker forwards no more than W", async () => {
    const page = new Page();
    try {
        await page.init();
        await page.start();
        const count = 2500;  // about 90 KB of answers, more than W, and less than what parks the gateway
        const replies = [];
        const asked = [];
        for (let i = 0; i < count; ++i) {
            asked.push(page.request("submit", {symbol: "MSFT", order: {side: "BUY", quantity: 1, price: 100 + (i % 50)}})
                .then((reply) => replies.push(reply)));
        }
        // Until every command has run, which status shows, and then a few more round trips.
        for (let records = 0; records < 12 + count;) {
            records = (await page.request("status")).shards[0].records;
        }
        for (let i = 0; i < 5; ++i) {
            await page.request("status");
        }
        // Every answer here is one 36-byte OrderAccepted, so the window holds a whole number of
        // them: as many as fit in W, and no more.
        const given = replies.reduce((sum, reply) => sum + reply.frameBytes, 0);
        assert.ok(replies.length < count, "some answers wait");
        assert.ok(given <= 65536, `the worker forwarded at most W, not ${given}`);
        assert.ok(given > 65536 - 36, `and filled it, at ${given}`);

        // Consuming lets the rest through.
        let consumed = 0;
        while (replies.length < count) {
            const owed = replies.reduce((sum, reply) => sum + reply.frameBytes, 0) - consumed;
            consumed += owed;
            page.worker.postMessage({type: "consumed", connection: "sandbox", bytes: owed});
            await page.request("status");
        }
        await Promise.all(asked);
        assert.equal(replies.length, count);
        assert.ok(replies.every((reply) => reply.ok && reply.events[0].type === "OrderAccepted"));
    } finally {
        await page.end();
    }
});
