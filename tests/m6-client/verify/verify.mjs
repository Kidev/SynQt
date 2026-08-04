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

// Chromium is the only engine that takes its rasteriser as a command line argument, and it
// brings SwiftShader with it. Firefox and WebKit take whatever GL the machine offers, which
// on a headless Linux runner means Mesa's llvmpipe and nothing at all if Mesa is not
// installed; that is a machine to provision, not a flag to pass, and graphicsRenderer below
// is what notices. They are given no arguments either way, so that a launch failure means
// the runtime is missing and never that it choked on an argument meant for another engine.
function launchOptions(browserType, withoutWebGl) {
    const options = { headless, args: [] };
    if (withoutWebGl) {
        // Firefox is the one engine that can be told to withhold WebGL through a
        // preference, which is what makes this case possible without a second machine.
        options.firefoxUserPrefs = { "webgl.disabled": true };
        return options;
    }
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

// What the engine will give Qt Quick to draw with, asked of the engine itself on a blank
// page, and empty when it has nothing.
//
// Qt Quick has no software fallback on WebAssembly: with no WebGL context the scene graph
// cannot be created, and QSGRenderLoop ends that with qFatal, which is emscripten's abort().
// A client that aborts does not merely fail to draw. abort() sets emscripten's ABORT flag,
// after which callUserCallback returns without running anything, so every callback queued
// through emscripten_async_call is dropped from then on. That is the first of the two hops
// in QEventDispatcherWasm::wakeUp(), so the page keeps its WebSocket, its timers and its
// property pushes (all delivered outside that queue) and loses posted events for the rest
// of its life. The visible result is a client that connects, updates and never delivers a
// queued call, which reads exactly like a transport bug and is not one.
//
// So ask first. An engine with no context cannot run a Qt Quick client at all, and saying
// that is worth more than measuring a client that will abort thirty seconds later. A
// headless Linux runner needs Mesa's software rasteriser installed for this to answer; the
// workflow installs it and pins LIBGL_ALWAYS_SOFTWARE so the answer does not depend on a
// GPU probe.
async function graphicsRenderer(browser) {
    const page = await browser.newPage();
    try {
        await page.goto("about:blank");
        return await page.evaluate(() => {
            const canvas = document.createElement("canvas");
            const context = canvas.getContext("webgl2") || canvas.getContext("webgl");
            return context ? String(context.getParameter(context.RENDERER)) : "";
        });
    } finally {
        await page.close();
    }
}

// Every line the page says, not only the sentinels this file matches on. The assertions
// still key off "M6 ..." (Qt prefixes QML console.log with "qml: ") and off the starvation
// shim's "M0PUMP ...", but what a dying client says is usually neither: this harness once
// reported a twenty-second wait for a posted event while the reason stood one line above
// it, in a qFatal that the sentinel filter dropped on the floor.
//
// `fatals` collects uncaught page errors separately, so a wait can end the moment the
// client is gone instead of running out its clock against a runtime that has stopped.
function openTab(context, name, logs, fatals) {
    return context.newPage().then((page) => {
        page.on("console", (msg) => {
            const text = msg.text();
            logs.push(text);
            if (process.env.VERBOSE) console.log(`  [${name}] ${text}`);
        });
        page.on("pageerror", (e) => {
            logs.push("PAGEERROR " + e.message);
            fatals.push(e.message);
        });
        return page;
    });
}

// An uncaught error in the client is never something to wait out. On WebAssembly it is
// usually emscripten's abort(), which Qt reaches through qFatal, and it is worse than the
// end of one operation: abort() sets emscripten's ABORT flag, after which callUserCallback
// drops every callback queued through emscripten_async_call. That is the first hop of
// QEventDispatcherWasm::wakeUp(), so from that moment the page keeps its socket, its timers
// and its property pushes and silently loses its posted-event queue for good. Reporting the
// wait that follows is reporting the second symptom of a client that died earlier.
async function waitFor(predicate, timeoutMs, label, fatals = []) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        if (predicate()) return true;
        if (fatals.length > 0) {
            throw new Error(`the client died before "${label}": ${fatals[0]}`);
        }
        await sleep(250);
    }
    throw new Error("timed out waiting for: " + label);
}

// The whole log up to a limit, and the limit said out loud when it bites. A tail alone hid
// the line that mattered once already: the starved case reports on what the shim did during
// startup, and 25 lines of counter telemetry is enough to push that off the top.
function dumpEvidence(name, logs) {
    const limit = 120;
    const shown = logs.slice(0, limit);
    console.log(`    --- ${logs.length} line(s) seen by ${name}` +
                (logs.length > limit ? `, first ${limit} shown` : "") + " ---");
    for (const line of shown) {
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
    const fatalsA = [];
    const fatalsB = [];
    const { proc: edge, port } = await startEdge();
    const url = `http://127.0.0.1:${port}/`;
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext();
        const tabA = await openTab(context, `${name}/tabA`, logsA, fatalsA);
        const tabB = await openTab(context, `${name}/tabB`, logsB, fatalsB);

        console.log("  loading two tabs against the real edge");
        await tabA.goto(url, { waitUntil: "load", timeout: 60000 });
        await tabB.goto(url, { waitUntil: "load", timeout: 60000 });

        const connectedAt = (logs, value) =>
            logs.some((l) => l.includes("state=connected") && l.includes(`counter=${value}`));

        await waitFor(() => connectedAt(logsA, 0), 60000, "tab A connected, counter=0",
                      fatalsA);
        await waitFor(() => connectedAt(logsB, 0), 60000, "tab B connected, counter=0",
                      fatalsB);
        console.log("  both tabs connected, counter=0");

        // The client posts one event at startup and says so when it is delivered (see
        // Main.qml). Asserting it here is what makes its absence mean something in the
        // starved case below: without a healthy run to compare against, "the message never
        // came" would be as consistent with a broken client as with a starved pump.
        await waitFor(() => logsA.some((l) => l.includes("posted-event pump alive")), 20000,
                      "tab A delivered its posted event", fatalsA);
        console.log("  the posted-event pump delivered");

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
                      "tab A counter=1", fatalsA);
        // The other tab shares the same edge-owned counter: it must see the new value.
        await waitFor(() => logsB.some((l) => l.includes("counter=1")), 20000,
                      "tab B counter=1 (sync)", fatalsB);
        console.log("  both tabs show counter=1; two tabs stay in sync");

        // Browser back, through the real popstate listener. The client pushed /about onto
        // the session history shortly after connecting (see Main.qml), so this is a genuine
        // history entry and not a document navigation: the page is not reloaded and the
        // client is the same instance, which is what makes the assertion about the listener
        // and not about a fresh boot. This path is WebAssembly-only, so no native test can
        // reach it.
        await waitFor(() => logsA.some((l) => l.includes("route=/about")), 20000,
                      "tab A navigated to /about", fatalsA);
        console.log("  tab A pushed /about; pressing the browser back button");
        const beforeBack = logsA.length;
        await tabA.goBack();
        await waitFor(() => logsA.slice(beforeBack).some((l) => l.includes("route=/")
                                                          && !l.includes("route=/about")),
                      20000, "tab A back on / after the browser back button", fatalsA);
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
    const fatals = [];
    const { proc: edge, port } = await startEdge();
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext();
        const tab = await openTab(context, name, logs, fatals);
        await tab.addInitScript({ path: pumpStarveShim });
        await tab.goto(`http://127.0.0.1:${port}/?starve=load`,
                       { waitUntil: "load", timeout: 60000 });

        await waitFor(() => logs.some((l) => l.includes("state=connected")), 60000,
                      "the starved client connected", fatals);
        console.log("  connected with the posted-event wakeup starved");

        // The page is starved from load, and the client posts one event as it comes up, so
        // the shim has something to drop before anything else happens here. Wait for the
        // drop rather than trying to provoke it: it is the shim's own record that the
        // wakeup was armed and taken away, and without it the rest of this case would be
        // measuring a perfectly healthy page.
        const wedged = () => logs.some((l) => l.includes("M0PUMP dropped"));
        await waitFor(wedged, 30000, "the shim to drop the posted-event wakeup", fatals);
        console.log("  the wakeup was dropped");

        // And the pump really is dead, not merely interfered with: the message the client
        // logs when its posted event is delivered never arrives. This is the assertion the
        // case is named for; the drop above is only how the state was reached.
        if (logs.some((l) => l.includes("posted-event pump alive"))) {
            throw new Error("the posted event was delivered anyway: the page was not starved");
        }

        // Click "+" until the client navigates. Clicks are delivered directly (window
        // system events are synchronous on WebAssembly), so this exercises the router
        // through a path that does not depend on the queue that has just been taken away.
        const canvas = tab.locator("canvas").first();
        const box = await canvas.boundingBox();
        if (!box || box.width === 0 || box.height === 0) {
            throw new Error("the client canvas has no box: nothing rendered");
        }
        const navigated = () => logs.some((l) => l.includes("route=/about"));
        for (let click = 0; click < 20 && !navigated(); ++click) {
            await tab.mouse.click(box.x + box.width / 2 + 24, box.y + box.height / 2 + 34);
            await sleep(500);
        }
        if (!navigated()) {
            throw new Error("the starved client never navigated to /about");
        }
        console.log("  the client pushed /about with its pump wedged");
        const beforeBack = logs.length;
        await tab.goBack();
        await waitFor(() => logs.slice(beforeBack).some((l) => l.includes("route=/")
                                                        && !l.includes("route=/about")),
                      20000, "the starved client went back to /", fatals);
        console.log("  the back button still reached the router");
        return { name, pass: true, logsA: logs, logsB: [] };
    } catch (err) {
        return { name, pass: false, error: err.message, logsA: logs, logsB: [] };
    } finally {
        await browser.close();
        edge.kill("SIGKILL");
    }
}

// The whole point of the fallback, in the one place it can be measured: an engine that
// hands out no WebGL context at all.
//
// Without it the client does not merely fail to draw. Qt Quick cannot create its scene
// graph, QSGRenderLoop calls qFatal, that is emscripten's abort(), and abort() sets ABORT,
// after which every callback queued through emscripten_async_call is dropped: the first
// hop of QEventDispatcherWasm::wakeUp(). The client keeps its socket and its timers and
// loses its posted-event queue for the rest of the page. This case asserts the opposite of
// all of that: it connects, it draws, it stays responsive, and the one route declared to
// need the accelerated pipeline says so instead of showing a blank area.
async function runWithoutWebGl(browserType, name) {
    const logs = [];
    const fatals = [];
    const { proc: edge, port } = await startEdge();
    const browser = await browserType.launch(launchOptions(browserType, true));
    try {
        const context = await browser.newContext();
        const tab = await openTab(context, name, logs, fatals);
        await tab.goto(`http://127.0.0.1:${port}/`, { waitUntil: "load", timeout: 60000 });

        await waitFor(() => logs.some((l) => l.includes("state=connected")), 60000,
                      "the client connected without WebGL", fatals);
        console.log("  connected with no WebGL context");

        // It knows it is on the raster adaptation, which is what every guard below reads.
        await waitFor(() => logs.some((l) => l.includes("softwareRendered=true")), 20000,
                      "the client to report software rendering", fatals);

        // The pump is the tell. A client that aborted would connect and then never deliver
        // this, which is exactly the failure this whole feature exists to prevent.
        await waitFor(() => logs.some((l) => l.includes("posted-event pump alive")), 20000,
                      "the posted-event pump to survive the missing context", fatals);
        console.log("  the posted-event pump is alive, so nothing aborted");

        // And it really rendered: the canvas has a box and a click reaches the button.
        const canvas = tab.locator("canvas").first();
        const box = await canvas.boundingBox();
        if (!box || box.width === 0 || box.height === 0) {
            throw new Error("the client canvas has no box: software rendering drew nothing");
        }
        await tab.mouse.click(box.x + box.width / 2 + 24, box.y + box.height / 2 + 34);
        await waitFor(() => logs.some((l) => l.includes("counter=1")), 20000,
                      "a click to reach the button on the raster adaptation", fatals);
        console.log("  it renders and a click reaches the counter");

        // The route that needs what this browser cannot give: refused, and named as such
        // (Router.Unsupported is 5). The visitor stays on the path they asked for.
        await tab.evaluate(() => window.history.pushState({}, "", "/3d"));
        await tab.evaluate(() => window.dispatchEvent(new PopStateEvent("popstate")));
        await waitFor(() => logs.some((l) => l.includes("route=/3d")), 20000,
                      "the client to navigate to the accelerated route", fatals);
        await waitFor(() => logs.some((l) => l.includes("pageStatus=5")), 20000,
                      "the accelerated route to report Unsupported", fatals);
        console.log("  the accelerated route showed the notice instead of a blank page");
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
    const unproven = [];
    for (const [browserType, browserName] of candidates) {
        let probe = null;
        try {
            probe = await browserType.launch(launchOptions(browserType));
        } catch (err) {
            console.log(`  skipping ${browserName}: ${firstLine(err)}`);
            continue;
        }
        try {
            versions[browserName] = probe.version();
            // A runtime that launches but cannot hand out a WebGL context runs no Qt Quick
            // client, so there is nothing here to measure through it. Said out loud and
            // carried into the summary rather than turned into a client failure thirty
            // seconds later: the fix is in the machine, not in the client.
            const renderer = await graphicsRenderer(probe);
            if (!renderer) {
                unproven.push([browserName, "no WebGL context, so Qt Quick cannot start"]);
                console.log(`  skipping ${browserName}: no WebGL context, so the Qt Quick `
                            + "client cannot start (install Mesa's software rasteriser)");
                continue;
            }
            versions[browserName] += ` on ${renderer}`;
            engines.push([browserType, browserName]);
        } finally {
            await probe.close();
        }
    }
    if (engines.length === 0) {
        throw new Error("no browser engine could run a Qt Quick client");
    }
    // Which engine build ran, and what it draws with, on every run and not only a failing
    // one: a pass against an unnamed engine cannot be compared with the next one.
    const engineList = engines.map(([, n]) => `${n} ${versions[n]}`).join(", ");
    console.log(`M6 verify: headless=${headless}  engines: ${engineList}`);

    const results = [];
    for (const [browserType, browserName] of engines) {
        const cases = [["", runCase], ["-starved", runStarvedCase]];
        // Only Firefox can be told to withhold WebGL, so only Firefox can run it.
        if (browserName === "firefox") {
            cases.push(["-nogl", runWithoutWebGl]);
        }
        for (const [suffix, run] of cases) {
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
    for (const [browserName, why] of unproven) {
        console.log(`  UNPROVEN  ${browserName} (${why})`);
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
