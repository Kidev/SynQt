// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The multi-threaded counterpart to verify-phys.mjs. The single-threaded build loads and
// boots but its PhysicsWorld never advances in a browser (the box stays at its release
// height); the working hypothesis is that Qt Quick 3D Physics steps PhysX on a worker thread
// that the single-threaded WASM kit cannot spawn, so simulate()/fetchResults() never
// completes. This harness settles that: it serves the wasm_multithread build under the
// cross-origin-isolation headers SharedArrayBuffer requires (COOP: same-origin, COEP:
// require-corp), confirms the page is actually isolated (crossOriginIsolated === true, a real
// SharedArrayBuffer, a non-empty pthread pool), and then samples the box height over several
// seconds to see whether the simulation now advances.
//
// Under fully headless automation the requestAnimationFrame loop can still idle, so a "did
// not advance" result here is not conclusive on its own; but "isolated + advanced" is a
// positive proof, and the script also prints the interactive URL so the same build can be
// watched falling in a real tab, which is the definitive check.

import { chromium } from "playwright";
import http from "node:http";
import fsp from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const clientDir = path.join(repoRoot, "build/q3dphys-wasm-mt");

const PORT = Number(process.env.PHYS_PORT || 8096);
const headless = process.env.PHYS_HEADLESS === "1" ? true : !process.env.DISPLAY;
const serveOnly = process.env.PHYS_SERVE === "1";

const MIME = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".wasm": "application/wasm",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".css": "text/css",
    ".ico": "image/x-icon",
    ".png": "image/png"
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The static server that makes multi-threaded WASM work at all: without these two headers the
// browser refuses to hand the page a SharedArrayBuffer, and the pthread-based Qt runtime
// cannot start its worker pool. This is exactly what the SynQt web edge emits when
// security.cross_origin_isolation is on.
function startIsolatedServer() {
    const server = http.createServer(async (req, res) => {
        const coi = {
            "Cross-Origin-Opener-Policy": "same-origin",
            "Cross-Origin-Embedder-Policy": "require-corp",
            "Cross-Origin-Resource-Policy": "same-origin"
        };
        try {
            const parsed = new URL(req.url, "http://127.0.0.1");
            let rel = decodeURIComponent(parsed.pathname);
            if (rel === "/") {
                rel = "/quick3dphys-wasm.html";
            }
            const file = path.join(clientDir, rel);
            if (!file.startsWith(clientDir)) {
                res.writeHead(403, coi);
                res.end();
                return;
            }
            const data = await fsp.readFile(file);
            const ext = path.extname(file).toLowerCase();
            res.writeHead(200, { ...coi, "Content-Type": MIME[ext] || "application/octet-stream" });
            res.end(data);
        } catch (err) {
            res.writeHead(404, coi);
            res.end(String(err));
        }
    });
    return new Promise((resolve) => {
        server.listen(PORT, "127.0.0.1", () => resolve(server));
    });
}

function launchOptions() {
    return {
        headless,
        args: [
            "--use-gl=angle",
            "--use-angle=swiftshader",
            "--enable-unsafe-swiftshader",
            "--ignore-gpu-blocklist"
        ]
    };
}

function parseLoad(logs) {
    for (const line of logs) {
        const m = line.match(/PHYS load status=(\d+) rootObjects=(\d+)/);
        if (m) {
            return { status: Number(m[1]), rootObjects: Number(m[2]) };
        }
    }
    return null;
}

// The lowest box height the scene has reported so far (main.cpp prints "PHYS y=<h> tick=<n>").
// A falling box drives this well below its release height of 200.
function lowestY(logs) {
    let low = Infinity;
    let ticks = 0;
    for (const line of logs) {
        const m = line.match(/PHYS y=([\-0-9.]+) tick=(\d+)/);
        if (m) {
            low = Math.min(low, Number(m[1]));
            ticks = Math.max(ticks, Number(m[2]));
        }
    }
    return { low, ticks };
}

async function main() {
    console.log(`PHYS-MT verify: headless=${headless} port=${PORT} serveOnly=${serveOnly}`);
    const server = await startIsolatedServer();
    if (serveOnly) {
        console.log(`\n  Serving the multi-threaded build with COOP/COEP at:`);
        console.log(`      http://127.0.0.1:${PORT}/`);
        console.log(`  Open it in a real browser tab and watch the box fall. Ctrl-C to stop.\n`);
        return; // keep the process (and the server) alive
    }
    const browser = await chromium.launch(launchOptions());
    const logs = [];
    try {
        const context = await browser.newContext();
        const page = await context.newPage();
        page.on("console", (msg) => {
            const text = msg.text();
            logs.push(text);
            if (process.env.VERBOSE && text.startsWith("PHYS")) {
                console.log("    " + text);
            }
        });
        page.on("pageerror", (err) => logs.push("PAGEERROR " + err.message));
        await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: "domcontentloaded",
            timeout: 60000 });

        // Wait for the scene to load and boot, then let it run so the frame loop has a chance
        // to step the simulation.
        let load = null;
        const start = Date.now();
        while (Date.now() - start < 90000) {
            load = parseLoad(logs);
            if (load && logs.some((l) => /PHYS started/.test(l))) {
                break;
            }
            await sleep(500);
        }
        // Give the simulation a generous window to advance under rAF.
        await sleep(8000);

        // Cross-origin isolation is the whole point of the multi-threaded kit: verify it took.
        const isolation = await page.evaluate(() => ({
            isolated: globalThis.crossOriginIsolated === true,
            hasSAB: typeof SharedArrayBuffer !== "undefined",
            hardwareConcurrency: navigator.hardwareConcurrency || 0
        }));

        const loaded = load !== null && load.status === 1 && load.rootObjects === 1;
        const enteredLoop = logs.some((l) => /PHYS exec/.test(l));
        const booted = logs.some((l) => /PHYS started/.test(l));
        const { low, ticks } = lowestY(logs);
        const advanced = low < 150; // released at 200; a real step drops it well under 150

        console.log("\n================== PHYS (WASM, MULTI-THREADED) ==================");
        console.log(`  cross-origin isolated (SharedArrayBuffer)  : ${isolation.isolated ? "PASS" : "FAIL"}` +
            `  (crossOriginIsolated=${isolation.isolated}, SAB=${isolation.hasSAB}, hc=${isolation.hardwareConcurrency})`);
        console.log(`  scene loads (Quick3D RHI + PhysX scene)    : ${loaded ? "PASS" : "FAIL"}` +
            (load ? `  (status=${load.status} rootObjects=${load.rootObjects})` : "  (no load line)"));
        console.log(`  event loop entered (PHYS exec)             : ${enteredLoop ? "PASS" : "FAIL"}`);
        console.log(`  event loop boots (PHYS started)            : ${booted ? "PASS" : "FAIL"}`);
        console.log(`  simulation advances (box falls)            : ${advanced ? "PASS" : "IDLE"}` +
            `  (lowestY=${Number.isFinite(low) ? low.toFixed(2) : "n/a"}, ticks=${ticks})`);
        console.log("================================================================");

        if (isolation.isolated && loaded && booted && advanced) {
            console.log("PHYS-MT GATE: GO; multi-threaded WASM runs PhysX and the box falls.");
            process.exit(0);
        }
        if (isolation.isolated && loaded && booted && !advanced) {
            console.log("PHYS-MT: isolated + booted, but the frame loop stayed idle under headless");
            console.log("automation. Re-run with PHYS_SERVE=1 and watch the printed URL in a real tab");
            console.log("to see whether the multi-threaded kit steps the simulation interactively.");
            process.exit(3);
        }
        // The classic freeze fingerprint: the scene loaded but the timer never ticked, so the
        // main thread stalled after (or during) app.exec(); the PhysX-worker deadlock on the
        // WASM pthread runtime that numThreads: 0 is meant to remove. Call it out explicitly so a
        // silent hang reads as a diagnosis, not a mystery.
        if (isolation.isolated && loaded && !booted) {
            console.log("  --> FROZE: the scene loaded but the event loop never ticked" +
                (enteredLoop ? " despite entering app.exec()" : " (never reached app.exec())") +
                ". This is the Qt Quick 3D Physics worker-thread deadlock on the WASM pthread");
            console.log("      runtime. Confirm PhysicsWorld.numThreads is 0 in Main.qml.");
        }
        const errs = logs.filter((l) => /PAGEERROR|abort|error/i.test(l)).slice(0, 5);
        for (const e of errs) {
            console.log("    log: " + e);
        }
        console.log("PHYS-MT GATE: NO-GO");
        process.exit(1);
    } finally {
        await browser.close();
        server.close();
    }
}

main().catch((err) => {
    console.error("PHYS-MT verify crashed:", err);
    process.exit(2);
});
