// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M0 go/no-go verification. Serves the built WASM client, runs the native edge,
// and drives real browsers (Chromium + Firefox) to assert all four QtRO directions
// over both ws and wss, plus reconnect after the edge restarts. Evidence is the
// browser console: the client emits single-line "M0 ..." sentinels this script
// matches. Exits 0 only if every required case passes.

import { chromium, firefox, webkit } from "playwright";
import http from "node:http";
import fs from "node:fs";
import fsp from "node:fs/promises";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const clientDir = path.join(repoRoot, "build/m0-client");
const edgeBin = path.join(repoRoot, "build/m0-edge/m0-edge");
const certFile = path.join(repoRoot, "build/certs/cert.pem");
const keyFile = path.join(repoRoot, "build/certs/key.pem");

const STATIC_PORT = 8080;
const WS_PORT = 8088;
const WSS_PORT = 8089;

// Headed against a real display when one is available; a real browser engine
// either way. Force headless with M0_HEADLESS=1.
const headless = process.env.M0_HEADLESS === "1" ? true : !process.env.DISPLAY;

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

async function waitFor(predicate, timeoutMs, stepMs = 300) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        if (predicate()) {
            return true;
        }
        await sleep(stepMs);
    }
    return false;
}

// Serve SynQt's loading page, not the Qt-branded one qt-cmake generates: it is the same
// renderer `synqt build` uses for a real app, so the page a visitor would see is the page
// these browsers actually boot from. Written over the build dir before the server starts.
function renderShell() {
    const result = spawnSync("python3",
        [path.join(repoRoot, "tools/wasm-shell.py"), "--target", "m0-client",
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
        server.listen(STATIC_PORT, "127.0.0.1", () => resolve(server));
    });
}

let edgeProc = null;

function startEdge() {
    return new Promise((resolve, reject) => {
        const args = ["--ws-port", String(WS_PORT), "--wss-port", String(WSS_PORT)];
        if (fs.existsSync(certFile) && fs.existsSync(keyFile)) {
            args.push("--cert", certFile, "--key", keyFile);
        }
        const proc = spawn(edgeBin, args, { stdio: ["ignore", "pipe", "pipe"] });
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

function launchOptions(browserType) {
    const options = { headless, args: [] };
    if (browserType === chromium) {
        options.args.push(
            "--ignore-certificate-errors",
            "--use-gl=angle",
            "--use-angle=swiftshader",
            "--enable-unsafe-swiftshader"
        );
    }
    return options;
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
        model: rows.some((n) => n >= 1),
        counters,
        rowsMax: rows.length ? Math.max(...rows) : 0
    };
}

async function waitForAllPaths(logs, timeoutMs = 45000) {
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

function pageUrl(scheme, port) {
    const target = `${scheme}://localhost:${port}`;
    return `http://127.0.0.1:${STATIC_PORT}/index.html?url=${encodeURIComponent(target)}`;
}

// What the page actually said, for a case that failed.
//
// Every one of these lines was already being collected and then dropped on the floor: a
// failure reported five booleans and a counter array, which say which path did not work and
// nothing whatsoever about why. That is not enough to act on -- firefox-on-Linux has failed
// here with reply=false through two rounds of investigation, and each round had to start by
// guessing what the page had done, because the one artefact that knew was discarded at the
// moment it became interesting. The console carries the client's own account (M0 slot error,
// M0 socket error=..., PAGEERROR ...), so print it where it is needed.
function dumpEvidence(logs) {
    const interesting = (logs || []).filter(
        (line) => /^(M0|PAGEERROR)/.test(line));
    // The frame-size instrument (M0 rx frame bytes=N) emits ~1-2 lines/sec, so a 25-line tail
    // would scroll the early reply frame out of view. The whole session is what makes the
    // "did the reply frame reach the client" question answerable, so show all of it on failure.
    const tail = interesting.slice(-400);
    if (!tail.length) {
        console.log("      (the page logged nothing at all)");
        return;
    }
    console.log(`      --- last ${tail.length} page log line(s) ---`);
    for (const line of tail) {
        console.log(`      | ${line}`);
    }
}

async function runCase(browserType, name, scheme, port) {
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const { page, logs } = await newPageWithLogs(context, name);
        await page.goto(pageUrl(scheme, port), { waitUntil: "load", timeout: 60000 });
        const result = await waitForAllPaths(logs);
        return { name, ...result, logs };
    } finally {
        await browser.close();
    }
}

async function runReconnect(browserType, name, scheme, port) {
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const { page, logs } = await newPageWithLogs(context, name);
        await page.goto(pageUrl(scheme, port), { waitUntil: "load", timeout: 60000 });

        const initial = await waitForAllPaths(logs);
        if (!initial.pass) {
            return { name, pass: false, stage: "initial", ...initial, logs };
        }

        const mark = logs.length;
        await stopEdge();
        const sawDisconnect = await waitFor(
            () => logs.slice(mark).some((l) => /M0 state=disconnected/.test(l)),
            15000
        );

        await startEdge();
        const sawReconnect = await waitFor(
            () => logs.slice(mark).some((l) => /M0 state=connected/.test(l)),
            30000
        );
        const reconnectMark = logs.length;
        const sawFreshData = await waitFor(
            () => logs.slice(reconnectMark).some((l) => /M0 prop counter=/.test(l)),
            15000
        );

        return {
            name,
            pass: sawDisconnect && sawReconnect && sawFreshData,
            sawDisconnect,
            sawReconnect,
            sawFreshData,
            logs
        };
    } finally {
        await browser.close();
    }
}

async function main() {
    console.log(`M0 verify: headless=${headless} (DISPLAY=${process.env.DISPLAY || "none"})`);
    renderShell();
    const staticServer = await startStaticServer();
    await startEdge();

    const results = [];
    // WebKit is Safari's engine, so Playwright's headless WebKit is the closest in-env proxy
    // for "does the QtRO-over-WebSockets path work in Safari" that a Linux box can run (real
    // Safari-on-macOS is still the final word). Each candidate is probed for launchability and
    // dropped with a note if its runtime is not installed, so a missing WebKit never fails the
    // gate: it just leaves Safari's engine unverified, exactly as before.
    const candidateBrowsers = [
        [chromium, "chromium"],
        [firefox, "firefox"],
        [webkit, "webkit"]
    ];
    const browsers = [];
    for (const [browserType, browserName] of candidateBrowsers) {
        try {
            const probe = await browserType.launch(launchOptions(browserType));
            await probe.close();
            browsers.push([browserType, browserName]);
        } catch (err) {
            console.log(`  skipping ${browserName}: ${String(err.message).split("\n")[0]}`);
        }
    }
    const ranWebkit = browsers.some(([, name]) => name === "webkit");
    const schemes = [
        ["ws", WS_PORT],
        ["wss", WSS_PORT]
    ];

    try {
        for (const [browserType, browserName] of browsers) {
            for (const [scheme, port] of schemes) {
                const name = `${browserName}-${scheme}`;
                process.stdout.write(`\n=== case ${name} ===\n`);
                const result = await runCase(browserType, name, scheme, port);
                results.push(result);
                console.log(
                    `    ${result.pass ? "PASS" : "FAIL"} connected=${result.connected} ` +
                        `prop=${result.prop} signal=${result.signal} reply=${result.reply} ` +
                        `model=${result.model} counters=[${result.counters.join(",")}] ` +
                        `rowsMax=${result.rowsMax}`
                );
                if (!result.pass) {
                    dumpEvidence(result.logs);
                }
            }
        }

        for (const [browserType, browserName] of browsers) {
            const name = `${browserName}-reconnect`;
            process.stdout.write(`\n=== case ${name} ===\n`);
            const result = await runReconnect(browserType, name, "ws", WS_PORT);
            results.push(result);
            console.log(
                `    ${result.pass ? "PASS" : "FAIL"} ` +
                    `disconnect=${result.sawDisconnect} reconnect=${result.sawReconnect} ` +
                    `freshData=${result.sawFreshData}` +
                    (result.stage ? ` stage=${result.stage}` : "")
            );
            if (!result.pass) {
                dumpEvidence(result.logs);
            }
        }
    } finally {
        await stopEdge();
        staticServer.close();
    }

    const failed = results.filter((r) => !r.pass);
    console.log("\n==================== M0 SUMMARY ====================");
    for (const r of results) {
        console.log(`  ${r.pass ? "PASS" : "FAIL"}  ${r.name}`);
    }
    console.log("===================================================");
    if (failed.length > 0) {
        console.log(`M0 GATE: NO-GO (${failed.length} failing case(s))`);
        process.exit(1);
    }
    const safariNote = ranWebkit
        ? "WebKit (Safari's engine) passed in-env; Safari-on-macOS is the final confirmation"
        : "Safari outstanding on macOS (WebKit runtime not installed here)";
    console.log(`M0 GATE: GO (all cases passed; ${safariNote})`);
    process.exit(0);
}

main().catch((err) => {
    console.error("M0 verify crashed:", err);
    stopEdge().finally(() => process.exit(2));
});
