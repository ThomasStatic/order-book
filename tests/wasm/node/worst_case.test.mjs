// The worst-case session (spec #127, "Testing Decisions"), at the shipped settings in the real
// engine worker under Node: the largest sample sent again and again, on one instrument of each
// shard, until several 64 KiB log files per shard have rolled, with a crash partway through. The
// crash is the page ending the worker without warning, once roll forward and retention have
// settled; a fresh worker is given the page store, verifies it, and starts, recovering from the
// checkpoints roll forward published. Nothing may abort, and the session must print exactly what
// the same session printed with no crash, the recovery's own lines saying where each shard
// recovered from (the checkpoint where its writer's file began) and to (the log's end). See docs/internals/testing.md and wasm.md.
import {test} from "node:test";
import assert from "node:assert/strict";

import {Page, PageStore} from "./page.mjs";

const SAMPLE = "valid/replace.csv";  // the largest sample, 13 commands
const SYMBOLS = ["AAPL", "NVDA"];     // one on each shard
const SENDS = 1400;                   // 700 a shard, about 9,100 records: five rolls of 1,636
const CRASH_AFTER = 700;              // two rolls a shard, so retention has deleted a file

const typed = (i) => `send ${SAMPLE} --symbol ${SYMBOLS[i % 2]}`;

async function play(page, from, to) {
    let transcript = "";
    for (let i = from; i < to; ++i) {
        transcript += await page.play([typed(i)]);
    }
    return transcript;
}

// Each shard's record count, from `status`.
function logEnds(text) {
    const ends = new Map();
    for (const match of text.matchAll(/^shard (\d+):.* records=(\d+)$/gm)) {
        ends.set(Number(match[1]), Number(match[2]));
    }
    return ends;
}

// What the store holds of one shard: its log files' first positions, and each book's checkpoints.
function shardFiles(store, shard, symbols) {
    const logs = [];
    const checkpoints = new Map(symbols.map((symbol) => [symbol, []]));
    for (const name of store.names()) {
        const log = /^log-(\d{4})-(\d{20})\.oblog$/.exec(name);
        if (log && Number(log[1]) === shard) {
            logs.push(Number(log[2]));
        }
        const checkpoint = /^ckpt-([A-Z.]+)-(\d{20})\.obckpt$/.exec(name);
        if (checkpoint && checkpoints.has(checkpoint[1])) {
            checkpoints.get(checkpoint[1]).push(Number(checkpoint[2]));
        }
    }
    logs.sort((a, b) => a - b);
    return {logs, checkpoints};
}

// The file the shard writes now begins at the last first position before its log end.
function writing(files, end) {
    return Math.max(...files.logs.filter((position) => position < end));
}

// Roll forward and retention have caught up: every book has a checkpoint where the writer's file
// begins, keeps at most two, and the oldest log file left is the oldest checkpoint any book keeps.
function settled(store, placement, ends) {
    return placement.every((symbols, shard) => {
        const files = shardFiles(store, shard, symbols);
        if (files.logs.length === 0) {
            return false;
        }
        const begins = writing(files, ends.get(shard));
        const kept = [...files.checkpoints.values()];
        const floor = Math.min(...kept.map((positions) => Math.min(...positions)));
        return kept.every((positions) => positions.includes(begins) && positions.length <= 2) && files.logs[0] === floor;
    });
}

test("the worst-case session rolls, crashes, recovers, and changes no byte", async () => {
    // The same session with no crash.
    const steady = new Page();
    let expected;
    let steadyStatus;
    try {
        assert.equal((await steady.init()).ok, true);
        await steady.start();
        expected = await play(steady, 0, SENDS);
        steadyStatus = await steady.play(["status"]);
        assert.deepEqual([(await steady.stop()).name], ["CLEAN_STOP"]);
    } finally {
        await steady.end();
    }

    const store = new PageStore();
    const first = new Page(store);
    let transcript;
    let ends;
    let placement;
    try {
        assert.equal((await first.init()).ok, true);
        placement = (await first.start()).placement;
        transcript = await play(first, 0, CRASH_AFTER);
        ends = logEnds(await first.play(["status"]));
        await first.until(() => settled(store, placement, ends));
    } finally {
        await first.end();  // the crash: no stop, and nothing it posts from now on counts
    }

    // Roll forward published checkpoints past the start, and retention deleted log files.
    const removed = first.posted.filter((message) => message.type === "remove" && /^log-/.test(message.name));
    for (const [shard, symbols] of placement.entries()) {
        const files = shardFiles(store, shard, symbols);
        assert.ok(files.logs[0] > 0, `shard ${shard}'s first log file was deleted: ${files.logs}`);
        assert.ok(removed.some((message) => message.name.startsWith(`log-000${shard}-`)), `shard ${shard} removed a log file`);
        assert.ok(writing(files, ends.get(shard)) >= 2 * 1636, `shard ${shard} rolled twice`);
    }

    const recovering = new Page(store);
    try {
        assert.equal((await recovering.init()).ok, true);

        // The dead store verifies, as the native recording does before its restart.
        const verified = await recovering.request("run", {line: "verify"});
        assert.deepEqual([verified.exit, verified.stderr], [0, ""], verified.stdout + verified.stderr);
        assert.ok(verified.stdout.endsWith("\nok\n"), verified.stdout);

        await recovering.start();
        assert.ok(!recovering.posted.some((message) => message.type === "seeded"), "a store with a log isn't seeded");
        const recovered = recovering.posted
            .filter((message) => message.type === "line" && message.stream === "diagnostics" && message.text.includes(": recovered "))
            .map((message) => message.text);
        const said = placement.map((symbols, shard) => {
            const begins = writing(shardFiles(store, shard, symbols), ends.get(shard));
            return `order_book_engine: shard ${shard}: recovered books=${symbols.length} replayed=${ends.get(shard)} ` +
                   `tail-cut-bytes=0 from=${begins} fell-back=0`;
        });
        assert.deepEqual(recovered, said);

        transcript += await play(recovering, CRASH_AFTER, SENDS);
        assert.equal(await recovering.play(["status"]), steadyStatus);
        assert.deepEqual([(await recovering.stop()).name], ["CLEAN_STOP"]);
    } finally {
        await recovering.end();
    }

    assert.ok(transcript === expected, "the crashed session prints exactly what the steady one did");
    const rolls = (status) => [...logEnds(status).values()].map((end) => Math.floor(end / 1636));
    assert.ok(rolls(steadyStatus).every((count) => count >= 5), `several files a shard rolled: ${steadyStatus}`);
});
