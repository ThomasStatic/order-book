// The module at its shipped settings, in the real engine worker under Node: every committed
// terminal session (tests/wasm/sessions/<name>.txt) played through run(line) on a first start,
// which seeds, must print <name>.expected, the transcript the native terminal test holds too.
// Also the protocol: init first, the version exchange, and the ordered channel. See
// docs/internals/testing.md and wasm.md.
import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync, readdirSync} from "node:fs";
import {fileURLToPath} from "node:url";
import path from "node:path";

import {Page, PROTOCOL_VERSION, linesOf, moduleUrls} from "./page.mjs";

const sessions = fileURLToPath(new URL("../sessions/", import.meta.url));

const names = readdirSync(sessions)
    .filter((file) => file.endsWith(".txt"))
    .map((file) => path.basename(file, ".txt"))
    .sort();

test("there are sessions to play", () => {
    assert.ok(names.length >= 5, `found ${names.join(", ")}`);
});

for (const name of names) {
    test(`session ${name}`, async () => {
        const page = new Page();
        try {
            const initialized = await page.init();
            assert.equal(initialized.ok, true, initialized.error);
            const ready = await page.start();
            assert.deepEqual(ready.placement, [["AAPL", "MSFT"], ["NVDA", "TSLA"]]);

            // A first start is on an empty store: the seed goes in before ready, unprinted.
            const types = page.posted.map((message) => message.type);
            const seeded = page.posted.find((message) => message.type === "seeded");
            assert.ok(seeded && seeded.exit === 0 && seeded.stderr === "", "the seed is answered whole");
            assert.ok(types.indexOf("seeded") < types.indexOf("ready"), "seeded comes before ready");
            assert.ok(types.includes("write"), "the seed's log writes reach the page");

            const typed = linesOf(readFileSync(path.join(sessions, `${name}.txt`), "utf8"));
            const transcript = await page.play(typed);
            assert.equal(transcript, readFileSync(path.join(sessions, `${name}.expected`), "utf8"));

            const exited = await page.stop();
            assert.deepEqual([exited.code, exited.name], [0, "CLEAN_STOP"]);
            assert.ok(page.posted.some((message) => message.type === "line" && message.stream === "report"),
                      "the stop report comes as lines");
        } finally {
            await page.end();
        }
    });
}

test("nothing is accepted before init, and a protocol mismatch is refused", async () => {
    const page = new Page();
    try {
        const early = await page.request("start");
        assert.deepEqual(early, {id: early.id, ok: false, error: "init comes first"});
        const mismatched = await page.request("init", {protocol: PROTOCOL_VERSION + 1, ...moduleUrls(), store: []});
        assert.deepEqual([mismatched.ok, mismatched.error, mismatched.protocol], [false, "protocol", PROTOCOL_VERSION]);
        const initialized = await page.init();
        assert.equal(initialized.ok, true);
        const again = await page.init();
        assert.deepEqual([again.ok, again.error], [false, "already initialized"]);
        await page.start();
        const twice = await page.request("start");
        assert.deepEqual([twice.ok, twice.error], [false, "the engine is already running"], "the worker refuses misuse too");
        await page.stop();
    } finally {
        await page.end();
    }
});

test("an answer never comes before the log write it waited for", async () => {
    const page = new Page();
    try {
        await page.init();
        await page.start();
        const before = page.posted.length;
        const answered = page.request("run", {line: "buy AAPL 1 90"});
        const result = await answered;
        assert.equal(result.stdout, "assigned id=7\nline 2: OrderAccepted id=7 side=BUY price=90 qty=1\n");
        // The reply came on the same channel as the store changes, so every write it waited for
        // was posted first.
        assert.ok(page.posted.slice(before).some((message) => message.type === "write"), "its log write came first");
        await page.stop();
    } finally {
        await page.end();
    }
});
