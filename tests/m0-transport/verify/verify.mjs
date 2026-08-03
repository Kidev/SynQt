// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M0 go/no-go verification. Serves the built WASM client, runs the native edge,
// and drives real browsers (Chromium + Firefox + WebKit) to assert all four QtRO
// directions over both ws and wss, plus reconnect after the edge restarts. Evidence is
// the browser console: the client emits single-line "M0 ..." sentinels this script
// matches. Exits 0 only if every required case passes.
//
// Real Safari.app is a separate driver (verify-safari.mjs): Playwright cannot drive it.
// Both reach their verdict through harness.mjs, so "passing" means one thing here.

import { chromium, firefox, webkit } from "playwright";
import {
    STATIC_PORT, WS_PORT, WSS_PORT,
    dumpEvidence, pageUrl, renderShell, startEdge, startStaticServer, stopEdge,
    waitFor, waitForAllPaths
} from "./harness.mjs";

// Headed against a real display when one is available; a real browser engine
// either way. Force headless with M0_HEADLESS=1.
const headless = process.env.M0_HEADLESS === "1" ? true : !process.env.DISPLAY;

// Playwright reports a missing runtime as a message whose first line is blank (the readable
// part is the banner under it), and a skip that prints no reason is indistinguishable from a
// skip nobody can act on. Take the first line that says something.
function firstLine(err) {
    const line = String(err.message)
        .split("\n")
        .map((text) => text.replace(/[─-╿]/g, "").trim())
        .find((text) => /[a-z]/i.test(text) && !/^browserType\.launch:?$/.test(text));
    return line || "the runtime would not launch and gave no reason";
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

// What the engine will give Qt Quick to draw with, asked of the engine itself on a blank
// page, and empty when it has nothing.
//
// The M0 client is an ApplicationWindow, and Qt Quick has no software fallback on
// WebAssembly: with no WebGL context the scene graph cannot be created and QSGRenderLoop
// ends that with qFatal, which is emscripten's abort(). That is not confined to drawing.
// abort() sets emscripten's ABORT flag, and callUserCallback then returns without running
// anything, so every callback queued through emscripten_async_call is dropped from that
// moment on. That is the first of the two hops in QEventDispatcherWasm::wakeUp(), so the
// client keeps its socket, its timers and its property pushes and never delivers another
// posted event -- and the one path here that rides a posted event is the returning slot's
// reply (QRemoteObjectPendingCallWatcher::finished is QtRO's lone queued connection). A
// machine with no GL therefore produces exactly "reply=false while everything else works",
// which is the signature this matrix chased for months. Ask the engine first, and say so.
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

async function runCase(browserType, name, scheme, port) {
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const { page, logs } = await newPageWithLogs(context, name);
        await page.goto(pageUrl(scheme, port), { waitUntil: "load", timeout: 60000 });
        const result = await waitForAllPaths(() => logs);
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

        const initial = await waitForAllPaths(() => logs);
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
    const staticServer = await startStaticServer({ port: STATIC_PORT });
    await startEdge();

    const results = [];
    // WebKit is Safari's engine, so Playwright's headless WebKit is the closest in-env proxy
    // for "does the QtRO-over-WebSockets path work in Safari" that a Linux box can run (real
    // Safari-on-macOS is verify-safari.mjs, and needs macOS). Each candidate is probed for
    // launchability and dropped with a note if its runtime is not installed, so a missing
    // WebKit never fails the gate: it just leaves Safari's engine unverified, as before.
    const candidateBrowsers = [
        [chromium, "chromium"],
        [firefox, "firefox"],
        [webkit, "webkit"]
    ];
    const browsers = [];
    const engineVersions = {};
    for (const [browserType, browserName] of candidateBrowsers) {
        let probe = null;
        try {
            probe = await browserType.launch(launchOptions(browserType));
        } catch (err) {
            console.log(`  skipping ${browserName}: ${firstLine(err)}`);
            continue;
        }
        try {
            engineVersions[browserName] = probe.version();
            // A runtime that launches but hands out no WebGL context cannot start this
            // client at all, so it has nothing to say about the transport. Dropped with its
            // reason, the same way a missing runtime is, because the fix is to install a
            // rasteriser on the machine and not to change anything here.
            const renderer = await graphicsRenderer(probe);
            if (!renderer) {
                console.log(`  skipping ${browserName}: no WebGL context, so the Qt Quick `
                            + "client cannot start (install Mesa's software rasteriser)");
                continue;
            }
            engineVersions[browserName] += ` on ${renderer}`;
            browsers.push([browserType, browserName]);
        } finally {
            await probe.close();
        }
    }
    // Which engine build ran, on every run and not only a failing one. This harness is the
    // standing evidence behind "the client runs in current Chrome, Firefox, and Safari", and
    // package.json floats Playwright on ^1.49, so each run resolves whatever engine build is
    // current that day. A pass against an unnamed engine is the same defect as a benchmark
    // baseline with no host recorded: it cannot be compared to the next one.
    const engineList = browsers
        .map(([, name]) => `${name} ${engineVersions[name]}`)
        .join(", ");
    console.log(`  engines: ${engineList}`);
    // With every candidate dropped there is no gate, only an empty summary that reads like a
    // pass. Say what happened instead.
    if (browsers.length === 0) {
        throw new Error("no browser engine here can run the client, so nothing was proven");
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
    // Repeated here on purpose: the line above the probe loop survives a crash, this one is in
    // the tail, which is the part of a CI log anyone actually reads.
    console.log(`  engines: ${engineList}`);
    for (const r of results) {
        console.log(`  ${r.pass ? "PASS" : "FAIL"}  ${r.name}`);
    }
    console.log("===================================================");
    if (failed.length > 0) {
        console.log(`M0 GATE: NO-GO (${failed.length} failing case(s))`);
        process.exit(1);
    }
    const safariNote = ranWebkit
        ? "WebKit (Safari's engine) passed in-env; run verify-safari.mjs on macOS for Safari itself"
        : "Safari outstanding on macOS (WebKit runtime not installed here)";
    console.log(`M0 GATE: GO (all cases passed; ${safariNote})`);
    process.exit(0);
}

main().catch((err) => {
    console.error("M0 verify crashed:", err);
    stopEdge().finally(() => process.exit(2));
});
