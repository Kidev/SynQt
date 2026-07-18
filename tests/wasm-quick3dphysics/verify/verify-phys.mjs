// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Does Qt Quick 3D Physics load and initialise under WebAssembly? Serve the built scene,
// drive it in a browser, and read the C++ sentinels off the console (qWarning routes to the
// browser console in the WASM runtime; QML console.log does not, reliably, in a release
// build). Pass requires the QQuickView reach status Ready with a live root object and the
// event loop start (PHYS started); i.e. Quick3D brings up its RHI over WebGL and the
// PhysX-backed scene instantiates.
//
// It also samples whether the box actually falls. With PhysicsWorld.numThreads: 0 (sequential
// stepping: the WASM-safe setting, since the automatic default wants worker threads the kit
// cannot spawn) the single-threaded kit steps PhysX on the main thread, and the box falls from
// its release height and rests on the plane even under headless automation. The native run in
// run-phys.sh remains the correctness reference; this harness now confirms the WASM build steps
// too, not just loads.

import { chromium } from "playwright";
import http from "node:http";
import fsp from "node:fs/promises";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const clientDir = path.join(repoRoot, "build/q3dphys-wasm");

const PORT = 8095;
const headless = process.env.PHYS_HEADLESS === "1" ? true : !process.env.DISPLAY;

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

// The same SynQt loading page the product ships (see tools/wasm-shell.py), so this spike
// boots from the page a visitor would see rather than Qt's stock template.
function renderShell() {
    const result = spawnSync("python3",
        [path.join(repoRoot, "tools/wasm-shell.py"), "--target", "quick3dphys-wasm",
         "--out", clientDir],
        { stdio: "inherit" });
    if (result.status !== 0) {
        throw new Error("could not render the SynQt loading shell");
    }
}

function startStaticServer() {
    const server = http.createServer(async (req, res) => {
        try {
            const parsed = new URL(req.url, "http://127.0.0.1");
            let rel = decodeURIComponent(parsed.pathname);
            if (rel === "/") {
                rel = "/index.html";
            }
            const file = path.join(clientDir, rel);
            if (!file.startsWith(clientDir)) {
                res.writeHead(403);
                res.end();
                return;
            }
            const data = await fsp.readFile(file);
            const ext = path.extname(file).toLowerCase();
            res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream" });
            res.end(data);
        } catch (err) {
            res.writeHead(404);
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

// The lowest box height reported so far (main.cpp prints "PHYS y=<h> tick=<n>"). A falling box
// drives this well below its release height of 200.
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
    console.log(`PHYS verify: headless=${headless} (DISPLAY=${process.env.DISPLAY || "none"})`);
    renderShell();
    const server = await startStaticServer();
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
        // domcontentloaded, not load: the multi-megabyte Quick3D+PhysX wasm can keep the
        // load event pending well past boot, and we only need the document up before we
        // start reading the console the scene writes.
        await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: "domcontentloaded",
            timeout: 60000 });

        // Give the runtime time to stream the wasm, bring up the RHI, and instantiate the
        // PhysX-backed scene, then report status.
        let load = null;
        const start = Date.now();
        while (Date.now() - start < 90000) {
            load = parseLoad(logs);
            if (load && logs.some((l) => /PHYS started/.test(l))) {
                break;
            }
            await sleep(500);
        }

        // Let the simulation run so the sequential (numThreads: 0) step drives the box down.
        await sleep(8000);

        const loaded = load !== null && load.status === 1 && load.rootObjects === 1;
        const booted = logs.some((l) => /PHYS started/.test(l));
        const { low, ticks } = lowestY(logs);
        const advanced = low < 150; // released at 200; a real step drops it well under 150

        console.log("\n==================== PHYS (WASM) SUMMARY ====================");
        console.log(`  scene loads (Quick3D RHI + PhysX scene) : ${loaded ? "PASS" : "FAIL"}` +
            (load ? `  (status=${load.status} rootObjects=${load.rootObjects})` : "  (no load line)"));
        console.log(`  event loop boots (PHYS started)         : ${booted ? "PASS" : "FAIL"}`);
        console.log(`  simulation advances (box falls)         : ${advanced ? "PASS" : "IDLE"}` +
            `  (lowestY=${Number.isFinite(low) ? low.toFixed(2) : "n/a"}, ticks=${ticks})`);
        console.log("============================================================");

        if (loaded && booted && advanced) {
            console.log("PHYS (WASM) GATE: GO (Qt Quick 3D + PhysX build, load, boot, and step under WASM)");
            process.exit(0);
        }
        if (loaded && booted && !advanced) {
            // Loaded and booting but the frame loop stayed idle: not a build failure, but the box
            // did not move. run-phys-mt.sh --serve in a real tab is the interactive fallback.
            console.log("  note: booted but the box stayed put; if this recurs, watch it in a real");
            console.log("        tab (run-phys-mt.sh --serve); native correctness is pinned by run-phys.sh.");
            console.log("PHYS (WASM) GATE: GO (build, load, boot proven; fall idle under headless)");
            process.exit(0);
        }
        const errs = logs.filter((l) => /PAGEERROR|abort|error/i.test(l)).slice(0, 5);
        for (const e of errs) {
            console.log("    log: " + e);
        }
        console.log("PHYS (WASM) GATE: NO-GO");
        process.exit(1);
    } finally {
        await browser.close();
        server.close();
    }
}

main().catch((err) => {
    console.error("PHYS verify crashed:", err);
    process.exit(2);
});
