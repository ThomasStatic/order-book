// The release asset (spec #127, "The zip"): order_book_wasm-v<version>.zip, unpacked with
// CMake's own tar, holds exactly the module, the worker, the client and its types, and
// manifest.json, whose every sha256 is its file's; and the module's .wasm and .js with the worker
// come to at most 400 KB (400,000 bytes) at gzip -9. See docs/internals/wasm.md and testing.md.
import {test} from "node:test";
import assert from "node:assert/strict";
import {execFileSync} from "node:child_process";
import {createHash} from "node:crypto";
import {mkdtempSync, readFileSync, readdirSync, rmSync} from "node:fs";
import {tmpdir} from "node:os";
import path from "node:path";
import {pathToFileURL} from "node:url";
import {gzipSync} from "node:zlib";

const zip = process.env.ORDER_BOOK_WASM_ZIP;
const budget = 400 * 1000;
const shipped = ["engine-client.d.ts", "engine-client.js", "engine-worker.js", "manifest.json", "order_book_wasm.js", "order_book_wasm.wasm"];

test("the zip holds its six files, each as its manifest says, within the budget", async () => {
    const unpacked = mkdtempSync(path.join(tmpdir(), "order-book-wasm-zip-"));
    try {
        execFileSync(process.env.CMAKE_COMMAND, ["-E", "tar", "xf", zip], {cwd: unpacked});
        assert.deepEqual(readdirSync(unpacked).sort(), shipped);

        const manifest = JSON.parse(readFileSync(path.join(unpacked, "manifest.json"), "utf8"));
        const client = await import(pathToFileURL(process.env.ORDER_BOOK_ENGINE_CLIENT).href);
        assert.equal(manifest.release, client.ENGINE_RELEASE, "the client names the release it ships in");
        assert.equal(path.basename(zip), `order_book_wasm-${manifest.release}.zip`);
        assert.equal(manifest.protocol, client.PROTOCOL_VERSION);
        assert.match(manifest.commit, /^([0-9a-f]{40}|unknown)$/);
        assert.ok(typeof manifest.tag === "string" && manifest.tag !== "", "git describe's tag");
        assert.deepEqual(Object.keys(manifest.files).sort(), shipped.filter((name) => name !== "manifest.json"));
        for (const [name, sha256] of Object.entries(manifest.files)) {
            const actual = createHash("sha256").update(readFileSync(path.join(unpacked, name))).digest("hex");
            assert.equal(actual, sha256, `${name}'s sha256`);
        }

        const gzipped = ["order_book_wasm.wasm", "order_book_wasm.js", "engine-worker.js"]
            .map((name) => gzipSync(readFileSync(path.join(unpacked, name)), {level: 9}).length)
            .reduce((sum, bytes) => sum + bytes, 0);
        assert.ok(gzipped <= budget, `the module and the worker take ${gzipped} bytes at gzip -9, past ${budget}`);
        console.log(`gzip -9: ${gzipped} of ${budget} bytes`);
    } finally {
        rmSync(unpacked, {recursive: true, force: true});
    }
});
