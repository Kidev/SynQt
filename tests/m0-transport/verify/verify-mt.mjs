// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Multi-threaded WASM proof (the CLIENT-2 gap): the same M0 client, built with the
// wasm_multithread kit, needs SharedArrayBuffer, which the browser only grants under
// cross-origin isolation (COOP: same-origin + COEP: require-corp; exactly the headers
// the M5 web edge emits when security.cross_origin_isolation is on). This harness:
//   1. serves the threaded client WITH those headers and asserts the page is
//      crossOriginIsolated, SharedArrayBuffer exists, the threaded runtime boots, and
//      all four QtRO-over-WebSocket paths still work;
//   2. serves the very same bundle WITHOUT the headers as a control and asserts the page
//      is NOT isolated; proving the headers are load-bearing, not incidental.
// Exits 0 only if both cases behave as required in every browser that ran.
//
// Every engine that launches is driven, because this claim is per-engine: cross-origin
// isolation and SharedArrayBuffer are browser policy, so Chromium passing says nothing about
// WebKit. A browser that cannot launch is skipped with a message (WebKit needs Debian/Ubuntu
// libraries the Arch dev box lacks, which is why CI is where its column gets filled in),
// never silently dropped: MT_BROWSERS=chromium,webkit narrows the set by hand.

import { chromium, firefox, webkit } from "playwright";
import http from "node:http";
import fsp from "node:fs/promises";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const clientDir = path.join(repoRoot, "build/m0-client-mt");
const edgeBin = path.join(repoRoot, "build/m0-edge/m0-edge");

const ISOLATED_PORT = 8090;   // serves with COOP/COEP
const PLAIN_PORT = 8091;      // serves the same files without the isolation headers
const WS_PORT = 8092;

const headless = process.env.MT_HEADLESS === "1" ? true : !process.env.DISPLAY;

const MIME = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".wasm": "application/wasm",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".css": "text/css",
    ".ico": "image/x-icon",
    ".png": "image/png"
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The same SynQt loading page the product ships (see tools/wasm-shell.py), so the
// multi-threaded kit boots from the page a visitor would see, not Qt's stock template.
function renderShell() {
    const result = spawnSync("python3",
        [path.join(repoRoot, "tools/wasm-shell.py"), "--target", "m0-client",
         "--out", clientDir],
        { stdio: "inherit" });
    if (result.status !== 0) {
        throw new Error("could not render the SynQt loading shell");
    }
}

function startStaticServer(port, isolate) {
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
            const headers = { "Content-Type": MIME[ext] || "application/octet-stream" };
            if (isolate) {
                // The pair that unlocks SharedArrayBuffer. COEP require-corp additionally
                // means every subresource (the .wasm, the worker) must be same-origin or
                // carry CORP; here everything is same-origin, so it loads.
                headers["Cross-Origin-Opener-Policy"] = "same-origin";
                headers["Cross-Origin-Embedder-Policy"] = "require-corp";
            }
            res.writeHead(200, headers);
            res.end(data);
        } catch (err) {
            res.writeHead(404);
            res.end(String(err));
        }
    });
    return new Promise((resolve) => {
        server.listen(port, "127.0.0.1", () => resolve(server));
    });
}

let edgeProc = null;

function startEdge() {
    return new Promise((resolve, reject) => {
        const proc = spawn(edgeBin, ["--ws-port", String(WS_PORT)],
            { stdio: ["ignore", "pipe", "pipe"] });
        let ready = false;
        const onData = (chunk) => {
            const text = chunk.toString();
            process.stdout.write("[edge] " + text);
            if (!ready && text.includes("M0 edge listening")) {
                ready = true;
                resolve(proc);
            }
        };
        proc.stdout.on("data", onData);
        proc.stderr.on("data", onData);
        proc.on("exit", (code) => {
            if (!ready) {
                reject(new Error("edge exited before becoming ready, code=" + code));
            }
        });
        setTimeout(() => {
            if (!ready) {
                reject(new Error("edge did not report listening within 10s"));
            }
        }, 10000);
        edgeProc = proc;
    });
}

function stopEdge() {
    return new Promise((resolve) => {
        if (!edgeProc || edgeProc.exitCode !== null) {
            edgeProc = null;
            resolve();
            return;
        }
        edgeProc.on("exit", () => {
            edgeProc = null;
            resolve();
        });
        edgeProc.kill("SIGKILL");
    });
}

function launchOptions() {
    return {
        headless,
        args: [
            "--ignore-certificate-errors",
            "--use-gl=angle",
            "--use-angle=swiftshader",
            "--enable-unsafe-swiftshader"
        ]
    };
}

function newPageWithLogs(context, name) {
    return context.newPage().then((page) => {
        const logs = [];
        page.on("console", (msg) => {
            const text = msg.text();
            logs.push(text);
            if (process.env.VERBOSE && text.startsWith("M0")) {
                console.log(`    [${name}] ${text}`);
            }
        });
        page.on("pageerror", (err) => logs.push("PAGEERROR " + err.message));
        return { page, logs };
    });
}

function analyze(logs) {
    const counters = logs
        .map((line) => line.match(/M0 prop counter=(\d+)/))
        .filter(Boolean)
        .map((m) => Number(m[1]));
    const rows = logs
        .map((line) => line.match(/M0 model rows=(\d+)/))
        .filter(Boolean)
        .map((m) => Number(m[1]));
    return {
        connected: logs.some((l) => /M0 state=connected/.test(l)),
        prop: new Set(counters).size >= 2,
        signal: logs.some((l) => /M0 signal payload=/.test(l)),
        reply: logs.some((l) => /M0 slot reply=echo:m0-ping/.test(l)),
        model: rows.some((n) => n >= 1)
    };
}

async function waitForAllPaths(logs, timeoutMs = 60000) {
    const start = Date.now();
    let last = analyze(logs);
    while (Date.now() - start < timeoutMs) {
        last = analyze(logs);
        if (last.connected && last.prop && last.signal && last.reply && last.model) {
            return { pass: true, ...last };
        }
        await sleep(500);
    }
    return { pass: false, ...last };
}

function pageUrl(port) {
    const target = `ws://localhost:${WS_PORT}`;
    return `http://127.0.0.1:${port}/index.html?url=${encodeURIComponent(target)}`;
}

// Read the browser's isolation state once the document is up. Poll briefly because the
// value is stable from first script but the page needs a tick to exist.
async function isolationState(page) {
    for (let i = 0; i < 40; i++) {
        const state = await page.evaluate(() => ({
            isolated: self.crossOriginIsolated === true,
            sab: typeof SharedArrayBuffer === "function"
        })).catch(() => null);
        if (state) {
            return state;
        }
        await sleep(250);
    }
    return { isolated: false, sab: false };
}

async function runIsolated(browserType) {
    const browser = await browserType.launch(launchOptions());
    try {
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const { page, logs } = await newPageWithLogs(context, "isolated");
        await page.goto(pageUrl(ISOLATED_PORT), { waitUntil: "load", timeout: 60000 });
        const iso = await isolationState(page);
        const paths = await waitForAllPaths(logs);
        return { iso, paths };
    } finally {
        await browser.close();
    }
}

async function runPlainControl(browserType) {
    const browser = await browserType.launch(launchOptions());
    try {
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const { page } = await newPageWithLogs(context, "plain");
        await page.goto(pageUrl(PLAIN_PORT), { waitUntil: "load", timeout: 60000 }).catch(() => {});
        const iso = await isolationState(page);
        return { iso };
    } finally {
        await browser.close();
    }
}

// The engines to drive: every one Playwright can launch here, or the subset MT_BROWSERS
// names. Probing by launching is what verify.mjs does, and it is the honest test: an engine
// that is installed but broken should be skipped loudly, not counted as a pass.
async function selectBrowsers() {
    const candidates = [[chromium, "chromium"], [firefox, "firefox"], [webkit, "webkit"]];
    const wanted = (process.env.MT_BROWSERS || "").split(",").map((s) => s.trim()).filter(Boolean);
    const selected = [];
    for (const [browserType, name] of candidates) {
        if (wanted.length > 0 && !wanted.includes(name)) {
            continue;
        }
        try {
            const probe = await browserType.launch(launchOptions());
            await probe.close();
            selected.push([browserType, name]);
        } catch (err) {
            console.log(`  skipping ${name}: ${String(err.message).split("\n")[0]}`);
        }
    }
    return selected;
}

async function main() {
    console.log(`MT verify: headless=${headless} (DISPLAY=${process.env.DISPLAY || "none"})`);
    const browsers = await selectBrowsers();
    if (browsers.length === 0) {
        console.log("MT GATE: NO-GO (no browser could be launched)");
        process.exit(1);
    }
    console.log(`  driving: ${browsers.map(([, name]) => name).join(", ")}`);

    renderShell();
    const isolatedServer = await startStaticServer(ISOLATED_PORT, true);
    const plainServer = await startStaticServer(PLAIN_PORT, false);
    await startEdge();

    const results = [];
    try {
        for (const [browserType, name] of browsers) {
            console.log(`\n=== ${name}: case isolated (COOP/COEP present) ===`);
            const isolated = await runIsolated(browserType);
            console.log(
                `    crossOriginIsolated=${isolated.iso.isolated} ` +
                    `SharedArrayBuffer=${isolated.iso.sab} | ` +
                    `connected=${isolated.paths.connected} prop=${isolated.paths.prop} ` +
                    `signal=${isolated.paths.signal} reply=${isolated.paths.reply} ` +
                    `model=${isolated.paths.model}`
            );

            console.log(`=== ${name}: case control (no isolation headers) ===`);
            const plain = await runPlainControl(browserType);
            console.log(`    crossOriginIsolated=${plain.iso.isolated} ` +
                `SharedArrayBuffer=${plain.iso.sab}`);
            results.push({ name, isolated, plain });
        }
    } finally {
        await stopEdge();
        isolatedServer.close();
        plainServer.close();
    }

    console.log("\n==================== MT SUMMARY ====================");
    let allOk = true;
    for (const { name, isolated, plain } of results) {
        const isolatedOk = isolated.iso.isolated && isolated.iso.sab && isolated.paths.pass;
        const controlOk = !plain.iso.isolated;   // the headers are what unlock isolation
        console.log(`  ${isolatedOk ? "PASS" : "FAIL"}  ${name} isolated: COI + SAB + threaded QtRO paths`);
        console.log(`  ${controlOk ? "PASS" : "FAIL"}  ${name} control: not isolated without the headers`);
        allOk = allOk && isolatedOk && controlOk;
    }
    console.log("===================================================");
    if (!allOk) {
        console.log("MT GATE: NO-GO");
        process.exit(1);
    }
    console.log(`MT GATE: GO (multi-threaded WASM runs under cross-origin isolation in ` +
        `${results.map((r) => r.name).join(", ")})`);
    process.exit(0);
}

main().catch((err) => {
    console.error("MT verify crashed:", err);
    stopEdge().finally(() => process.exit(2));
});
