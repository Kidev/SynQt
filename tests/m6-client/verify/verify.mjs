// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M6 browser end-to-end: the WASM counter client runs in a real browser against the
// real web edge, and two tabs stay in sync. The QML renders to a canvas, so the client
// surfaces "M6 state=... counter=..." to the console (Main.qml telemetry) and this
// harness asserts on those sentinels and drives the "+" button by clicking the canvas.
//
// It runs on every engine whose runtime is installed. The transport underneath is proven
// engine by engine in tests/m0-transport; this is the layer above it (the client runtime,
// the QML canvas, the pointer event, and the fan-out to a second tab), which is why it
// cannot be left to one engine either. A missing runtime is skipped with a note rather
// than passed over in silence: WebKit in particular is not installed on every machine.

import { chromium, firefox, webkit } from "playwright";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const edgeBin = path.join(repoRoot, "build/m6-app-desktop/counter-edge");
const bundleDir = path.join(repoRoot, "build/m6-app-wasm");
const counterQml = path.join(repoRoot, "tests/m6-client/web/Counter.qml");
// Shared with the M0 spike, not copied: one definition of what a starved posted-event
// pump is, so the client runtime and the transport spike are measured against the same
// thing. See runStarvedCase below.
const pumpStarveShim = path.join(repoRoot, "tests/m0-transport/verify/pump-starve.js");

const headless = process.env.M6_HEADLESS === "1" ? true : !process.env.DISPLAY;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function startEdge() {
    return new Promise((resolve, reject) => {
        // The edge is a QGuiApplication (it runs a QML engine), so on a machine with no
        // display it would try the xcb plugin and abort before it ever listens. It draws
        // nothing, so the offscreen platform is right everywhere and not only in CI; hard
        // coding it here keeps the harness independent of whether a DISPLAY happens to be
        // set, while an explicit QT_QPA_PLATFORM still wins for anyone debugging it.
        const proc = spawn(edgeBin, [
            `--bundle=${bundleDir}`, `--counter-qml=${counterQml}`, "--port=0",
        ], {
            stdio: ["ignore", "pipe", "pipe"],
            env: { QT_QPA_PLATFORM: "offscreen", ...process.env },
        });
        let done = false;
        const onData = (chunk) => {
            const text = chunk.toString();
            process.stdout.write("[edge] " + text);
            const match = text.match(/http:\/\/127\.0\.0\.1:(\d+)/);
            if (!done && match) {
                done = true;
                resolve({ proc, port: Number(match[1]) });
            }
        };
        proc.stdout.on("data", onData);
        proc.stderr.on("data", onData);
        proc.on("exit", (code) => { if (!done) reject(new Error("edge exited early: " + code)); });
        setTimeout(() => { if (!done) reject(new Error("edge did not report listening")); }, 10000);
    });
}

// Only Chromium needs to be told how to get a GL context headless; Firefox and WebKit
// bring their own software path. They are not given the flags anyway, so that a launch
// failure means the runtime is missing and never that it choked on an argument meant
// for another engine.
function launchOptions(browserType) {
    const options = { headless, args: [] };
    if (browserType === chromium) {
        options.args.push("--use-gl=angle", "--use-angle=swiftshader",
                          "--enable-unsafe-swiftshader");
    }
    return options;
}

// Playwright reports a missing runtime as a message whose first line is blank (the readable
// part is the banner under it), and a skip that prints no reason is indistinguishable from a
// skip nobody can act on. Take the first line that says something.
function firstLine(err) {
    const line = String(err.message)
        .split("\n")
        .map((text) => text.replace(/[\u2500-\u257f]/g, "").trim())
        .find((text) => /[a-z]/i.test(text) && !/^browserType\.launch:?$/.test(text));
    return line || "the runtime would not launch and gave no reason";
}

function openTab(context, name, logs) {
    return context.newPage().then((page) => {
        page.on("console", (msg) => {
            const text = msg.text();
            // Qt prefixes QML console.log with "qml: ". M0PUMP lines come from the
            // starvation shim below, and are kept so a starved case can prove it really
            // was starved rather than pass because nothing was ever dropped.
            if (text.includes("M6 ") || text.includes("M0PUMP")) {
                logs.push(text);
                if (process.env.VERBOSE) console.log(`  [${name}] ${text}`);
            }
        });
        page.on("pageerror", (e) => logs.push("PAGEERROR " + e.message));
        return page;
    });
}

async function waitFor(predicate, timeoutMs, label) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        if (predicate()) return true;
        await sleep(250);
    }
    throw new Error("timed out waiting for: " + label);
}

function dumpEvidence(name, logs) {
    console.log(`    --- last lines seen by ${name} ---`);
    for (const line of logs.slice(-25)) {
        console.log(`      ${line}`);
    }
    if (logs.length === 0) {
        console.log("      (nothing: the client never reached its first telemetry line)");
    }
}

// One engine, one edge. The edge owns the counter, so each case gets its own process and
// starts from zero: sharing one edge across engines would make every case after the first
// assert on a value the previous case left behind.
async function runCase(browserType, name) {
    const logsA = [];
    const logsB = [];
    const { proc: edge, port } = await startEdge();
    const url = `http://127.0.0.1:${port}/`;
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext();
        const tabA = await openTab(context, `${name}/tabA`, logsA);
        const tabB = await openTab(context, `${name}/tabB`, logsB);

        console.log("  loading two tabs against the real edge");
        await tabA.goto(url, { waitUntil: "load", timeout: 60000 });
        await tabB.goto(url, { waitUntil: "load", timeout: 60000 });

        const connectedAt = (logs, value) =>
            logs.some((l) => l.includes("state=connected") && l.includes(`counter=${value}`));

        await waitFor(() => connectedAt(logsA, 0), 60000, "tab A connected, counter=0");
        await waitFor(() => connectedAt(logsB, 0), 60000, "tab B connected, counter=0");
        console.log("  both tabs connected, counter=0");

        // Drive the "+" button on tab A's canvas. The window is 320x220, content centred,
        // so the "+" (right button in the row) sits right of centre, below the label.
        console.log("  clicking + on tab A");
        const canvas = tabA.locator("canvas").first();
        const box = await canvas.boundingBox();
        if (!box || box.width === 0 || box.height === 0) {
            throw new Error("the client canvas has no box: nothing rendered");
        }
        await tabA.mouse.click(box.x + box.width / 2 + 24, box.y + box.height / 2 + 34);

        await waitFor(() => logsA.some((l) => l.includes("counter=1")), 20000,
                      "tab A counter=1");
        // The other tab shares the same edge-owned counter: it must see the new value.
        await waitFor(() => logsB.some((l) => l.includes("counter=1")), 20000,
                      "tab B counter=1 (sync)");
        console.log("  both tabs show counter=1; two tabs stay in sync");

        // Browser back, through the real popstate listener. The client pushed /about onto
        // the session history shortly after connecting (see Main.qml), so this is a genuine
        // history entry and not a document navigation: the page is not reloaded and the
        // client is the same instance, which is what makes the assertion about the listener
        // and not about a fresh boot. This path is WebAssembly-only, so no native test can
        // reach it.
        await waitFor(() => logsA.some((l) => l.includes("route=/about")), 20000,
                      "tab A navigated to /about");
        console.log("  tab A pushed /about; pressing the browser back button");
        const beforeBack = logsA.length;
        await tabA.goBack();
        await waitFor(() => logsA.slice(beforeBack).some((l) => l.includes("route=/")
                                                          && !l.includes("route=/about")),
                      20000, "tab A back on / after the browser back button");
        console.log("  back returned the client to /");
        return { name, pass: true, logsA, logsB };
    } catch (err) {
        return { name, pass: false, error: err.message, logsA, logsB };
    } finally {
        await browser.close();
        edge.kill("SIGKILL");
    }
}

// The same run with Qt's posted-event pump starved, which is what the client's back button
// and its deferred deletions were made not to depend on.
//
// Qt for WebAssembly delivers posted events (QEvent::MetaCall behind a queued connection,
// QEvent::DeferredDelete behind deleteLater) through one chain of two zero-delay browser
// callbacks, and does not re-arm it while one is pending, so a single lost callback stops
// that delivery for the life of the page while timers, sockets and property updates go on
// working. tests/m0-transport/FIREFOX-LINUX.md is the investigation; the shim reproduces it
// deterministically in any engine by dropping exactly the timeout the wakeup arms.
//
// The shim is shared with the M0 spike rather than copied, so there is one definition of
// what "starved" means. It is injected before Qt boots and reads its mode from the query
// string, which is why it is both an init script and a URL parameter.
async function runStarvedCase(browserType, name) {
    const logs = [];
    const { proc: edge, port } = await startEdge();
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext();
        const tab = await openTab(context, name, logs);
        await tab.addInitScript({ path: pumpStarveShim });
        await tab.goto(`http://127.0.0.1:${port}/?starve=load`,
                       { waitUntil: "load", timeout: 60000 });

        await waitFor(() => logs.some((l) => l.includes("state=connected")), 60000,
                      "the starved client connected");
        console.log("  connected with the posted-event wakeup starved");

        // Click "+" until the client has navigated AND the shim has actually dropped a
        // wakeup. Both conditions matter and neither implies the other: without the drop
        // the page is simply healthy and the case proves nothing, and the drop has to be
        // in the past before Back is pressed or Back would be tested against a working
        // pump. Clicking is what keeps the client posting events for the shim to catch.
        const canvas = tab.locator("canvas").first();
        const box = await canvas.boundingBox();
        if (!box || box.width === 0 || box.height === 0) {
            throw new Error("the client canvas has no box: nothing rendered");
        }
        const navigated = () => logs.some((l) => l.includes("route=/about"));
        const wedged = () => logs.some((l) => l.includes("M0PUMP dropped"));
        for (let click = 0; click < 20 && !(navigated() && wedged()); ++click) {
            await tab.mouse.click(box.x + box.width / 2 + 24, box.y + box.height / 2 + 34);
            await sleep(500);
        }
        if (!wedged()) {
            throw new Error("the wakeup was never dropped, so the page was never starved");
        }
        if (!navigated()) {
            throw new Error("the starved client never navigated to /about");
        }
        console.log("  the wakeup was dropped and the client pushed /about");
        const beforeBack = logs.length;
        await tab.goBack();
        await waitFor(() => logs.slice(beforeBack).some((l) => l.includes("route=/")
                                                        && !l.includes("route=/about")),
                      20000, "the starved client went back to /");
        console.log("  the back button still reached the router");
        return { name, pass: true, logsA: logs, logsB: [] };
    } catch (err) {
        return { name, pass: false, error: err.message, logsA: logs, logsB: [] };
    } finally {
        await browser.close();
        edge.kill("SIGKILL");
    }
}

async function main() {
    // Probed rather than assumed, the same way tests/m0-transport/verify/verify.mjs does
    // it: an engine whose runtime is not installed leaves its column unproven and says so,
    // and installing the runtime is the only step needed to cover it.
    const candidates = [
        [chromium, "chromium"],
        [firefox, "firefox"],
        [webkit, "webkit"]
    ];
    const engines = [];
    const versions = {};
    for (const [browserType, browserName] of candidates) {
        try {
            const probe = await browserType.launch(launchOptions(browserType));
            versions[browserName] = probe.version();
            await probe.close();
            engines.push([browserType, browserName]);
        } catch (err) {
            console.log(`  skipping ${browserName}: ${firstLine(err)}`);
        }
    }
    if (engines.length === 0) {
        throw new Error("no browser engine could be launched");
    }
    // Which engine build ran, on every run and not only a failing one: a pass against an
    // unnamed engine cannot be compared with the next one.
    const engineList = engines.map(([, n]) => `${n} ${versions[n]}`).join(", ");
    console.log(`M6 verify: headless=${headless}  engines: ${engineList}`);

    const results = [];
    for (const [browserType, browserName] of engines) {
        for (const [suffix, run] of [["", runCase], ["-starved", runStarvedCase]]) {
            const caseName = `${browserName}${suffix}`;
            console.log(`\n=== case ${caseName} ===`);
            const result = await run(browserType, caseName);
            results.push(result);
            console.log(`    ${result.pass ? "PASS" : "FAIL"} ${result.name}` +
                        (result.error ? ` -- ${result.error}` : ""));
            if (!result.pass) {
                dumpEvidence("tabA", result.logsA);
                dumpEvidence("tabB", result.logsB);
            }
        }
    }

    console.log("\n==================== M6 SUMMARY ====================");
    console.log(`  engines: ${engineList}`);
    for (const result of results) {
        console.log(`  ${result.pass ? "PASS" : "FAIL"}  ${result.name}`);
    }
    console.log("====================================================");
    const failed = results.filter((r) => !r.pass);
    if (failed.length > 0) {
        console.log(`M6 BROWSER: FAIL (${failed.length} failing engine(s))`);
        process.exit(1);
    }
    console.log("M6 BROWSER: PASS");
    process.exit(0);
}

main().catch((err) => {
    console.error("\nM6 BROWSER: FAIL --", err.message);
    process.exit(1);
});
