// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The parts of the M0 proof that are not about a particular browser driver: serving the built
// client, running the native edge, and deciding from the page's own log lines whether all four
// QtRO directions worked.
//
// Split out when real Safari joined the matrix. Safari cannot be driven by Playwright and its
// WebDriver exposes no console log, so it needs a different driver (verify-safari.mjs) but must
// reach exactly the same verdict from exactly the same sentinels. Two copies of `analyze` would
// be two definitions of what passing means, and the one that drifted would be the one nobody
// re-read.

import http from "node:http";
import fs from "node:fs";
import fsp from "node:fs/promises";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

export const here = path.dirname(fileURLToPath(import.meta.url));
export const repoRoot = path.resolve(here, "../../..");
// M0_CLIENT_DIR points the harness at a second build of the same client, which is how two
// builds that differ in one compile option get measured by one driver against one edge.
export const clientDir = process.env.M0_CLIENT_DIR
    ? path.resolve(process.env.M0_CLIENT_DIR)
    : path.join(repoRoot, "build/m0-client");
export const edgeBin = path.join(repoRoot, "build/m0-edge/m0-edge");
export const certFile = path.join(repoRoot, "build/certs/cert.pem");
export const keyFile = path.join(repoRoot, "build/certs/key.pem");

export const STATIC_PORT = 8080;
export const WS_PORT = 8088;
export const WSS_PORT = 8089;

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

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The predicate is awaited, so it may be sync or async. This matters: the Playwright cases
// test an array they already hold, while the Safari cases have to ask the browser over
// WebDriver and so can only answer with a promise, and an un-awaited promise is truthy,
// which would make every wait here return true on its first attempt and every Safari case
// pass without observing anything.
export async function waitFor(predicate, timeoutMs, stepMs = 300) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        if (await predicate()) {
            return true;
        }
        await sleep(stepMs);
    }
    return false;
}

// Serve SynQt's loading page, not the Qt-branded one qt-cmake generates: it is the same
// renderer `synqt build` uses for a real app, so the page a visitor would see is the page
// these browsers actually boot from. Written over the build dir before the server starts.
export function renderShell() {
    const result = spawnSync("python3",
        [path.join(repoRoot, "tools/wasm-shell.py"), "--target", "m0-client",
         "--out", clientDir],
        { stdio: "inherit" });
    if (result.status !== 0) {
        throw new Error("could not render the SynQt loading shell");
    }
}

// The console tap goes in ahead of everything else the page loads, because the sentinels it
// exists to catch start arriving as soon as the Qt runtime does. Injected here rather than
// written into the build directory so the bundle under test stays exactly what the build
// produced, and served from the verify directory as a same-origin file rather than inlined so
// that a strict `script-src 'self'` accepts it.
function injectTap(html, extraScripts) {
    const routes = ["/__synqt-console-tap.js", ...extraScripts.map((s) => s.route)];
    const tag = routes.map((route) => `<script src="${route}"></script>`).join("\n");
    if (html.includes("<head>")) {
        return html.replace("<head>", `<head>\n${tag}`);
    }
    return tag + html;
}

/**
 * `extraScripts` is a list of `{ route, file }` served same-origin and injected into the
 * page's head after the console tap, for a driver that needs to instrument the page before
 * Qt boots. The gate (verify.mjs) passes none, so the page it measures stays exactly what
 * the build produced.
 */
export function startStaticServer({ port = STATIC_PORT, headers = {}, extraScripts = [] } = {}) {
    const server = http.createServer(async (req, res) => {
        try {
            const parsed = new URL(req.url, "http://127.0.0.1");
            let rel = decodeURIComponent(parsed.pathname);
            if (rel === "/__synqt-console-tap.js") {
                const tap = await fsp.readFile(path.join(here, "console-tap.js"));
                res.writeHead(200, { ...headers, "Content-Type": "text/javascript" });
                res.end(tap);
                return;
            }
            const extra = extraScripts.find((script) => script.route === rel);
            if (extra) {
                const body = await fsp.readFile(path.join(here, extra.file));
                res.writeHead(200, { ...headers, "Content-Type": "text/javascript" });
                res.end(body);
                return;
            }
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
            if (ext === ".html") {
                const html = injectTap(data.toString("utf8"), extraScripts);
                res.writeHead(200, { ...headers, "Content-Type": "text/html" });
                res.end(html);
                return;
            }
            res.writeHead(200,
                { ...headers, "Content-Type": MIME[ext] || "application/octet-stream" });
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

export function startEdge() {
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

export function stopEdge() {
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

export function analyze(logs) {
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

/**
 * Poll until every path has been seen or the deadline passes. `readLogs` returns the log lines
 * known so far; Playwright accumulates them by event, Safari is asked for them over WebDriver,
 * and both go through this one definition of "all four worked".
 */
export async function waitForAllPaths(readLogs, timeoutMs = 45000) {
    const start = Date.now();
    let last = analyze(await readLogs());
    while (Date.now() - start < timeoutMs) {
        last = analyze(await readLogs());
        if (last.connected && last.prop && last.signal && last.reply && last.model) {
            return { pass: true, ...last };
        }
        await sleep(500);
    }
    return { pass: false, ...last };
}

export function pageUrl(scheme, port) {
    const target = `${scheme}://localhost:${port}`;
    return `http://127.0.0.1:${STATIC_PORT}/index.html?url=${encodeURIComponent(target)}`;
}

// What the page actually said, for a case that failed.
//
// Every one of these lines was already being collected and then dropped on the floor: a
// failure reported five booleans and a counter array, which say which path did not work and
// nothing whatsoever about why. That is not enough to act on; firefox-on-Linux has failed
// here with reply=false through two rounds of investigation, and each round had to start by
// guessing what the page had done, because the one artefact that knew was discarded at the
// moment it became interesting. The console carries the client's own account (M0 slot error,
// M0 socket error=..., PAGEERROR ...), so print it where it is needed.
export function dumpEvidence(logs) {
    // Every line, with nothing filtered. This used to keep only lines beginning with a
    // sentinel, which threw away the ones that name a cause: Qt says why it is about to call
    // qFatal in ordinary warnings ("Failed to create RHI", "Failed to initialize graphics
    // backend for OpenGL"), and the abort that follows reaches the harness as a bare
    // "PAGEERROR Aborted()" with nothing attached to it. Keeping only the sentinels turns a
    // named cause into an unexplained abort, which is exactly how long this one lasted.
    const interesting = logs || [];
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
