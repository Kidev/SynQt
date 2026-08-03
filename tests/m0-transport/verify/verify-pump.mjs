// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Reproduces, on any machine and in any engine, the condition that fails only in Firefox on
// GitHub's hosted Ubuntu runner: a returning slot whose reply arrives, decodes and resolves,
// while QRemoteObjectPendingCallWatcher::finished never fires.
//
// The investigation in ../FIREFOX-LINUX.md narrowed that failure to one mechanism: QtRO's
// only non-blocking completion path is a Qt::QueuedConnection
// (qremoteobjectpendingcall.cpp:30-35), so it needs a posted QEvent::MetaCall to be
// delivered, and in that environment it is not. Everything else on the socket keeps working
// because property, signal and model updates are activated directly from the read callback,
// and because Qt timers are delivered with sendEvent() rather than through the posted-event
// queue.
//
// So rather than chase the environment, this reproduces the mechanism: pump-starve.js drops
// the zero-delay browser timeout that QEventDispatcherWasm::wakeUp() arms, and only that
// one. The result is a page whose socket, timers and heartbeat all run normally and whose
// posted events never arrive, which is the CI signature exactly.
//
// Run it with the expectation you are testing:
//
//   node verify-pump.mjs stall     # stock Qt: the watcher must never fire
//   node verify-pump.mjs recover   # patched Qt: the watcher must fire anyway
//
// Both directions matter. "stall" is what proves the reproduction is real rather than a
// harness that always passes, and it is the failing test a fix has to turn green.

import { chromium, firefox, webkit } from "playwright";
import {
    STATIC_PORT, WS_PORT,
    pageUrl, renderShell, startEdge, startStaticServer, stopEdge, waitFor
} from "./harness.mjs";

const headless = process.env.M0_HEADLESS === "1" ? true : !process.env.DISPLAY;

const expectation = process.argv[2] || "stall";
if (expectation !== "stall" && expectation !== "recover") {
    console.error(`usage: node verify-pump.mjs [stall|recover]`);
    process.exit(2);
}

// The window has to outlast the client's own 2 s echo retry several times over, so that a
// verdict of "the watcher never fired" means it never fired rather than that we stopped
// looking too early. CI saw 45 unanswered invocations across 45 s.
const CASE_TIMEOUT_MS = 25000;

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

// The client resolves its echo two ways and says which one won, so the log distinguishes
// "the reply data arrived" from "the queued completion signal was delivered". That
// distinction is the whole measurement here: the poll line proves the reply is sitting in
// the call fully decoded, and the absence of the watcher line proves the caller was never
// told.
function analyzePump(logs) {
    const counters = logs
        .map((line) => line.match(/M0 prop counter=(\d+)/))
        .filter(Boolean)
        .map((m) => Number(m[1]));
    const rows = logs
        .map((line) => line.match(/M0 model rows=(\d+)/))
        .filter(Boolean)
        .map((m) => Number(m[1]));
    const replies = logs.filter((line) => /M0 slot reply=echo:m0-ping/.test(line));
    return {
        starving: logs.some((l) => /M0PUMP starving/.test(l)),
        dropped: logs.filter((l) => /M0PUMP dropped/.test(l)).length > 0,
        connected: logs.some((l) => /M0 state=connected/.test(l)),
        prop: new Set(counters).size >= 2,
        signal: logs.some((l) => /M0 signal payload=/.test(l)),
        model: rows.some((n) => n >= 1),
        // The reply data reached the client and resolved the call's own state.
        pollReply: replies.some((l) => /via poll fallback/.test(l)),
        // QRemoteObjectPendingCallWatcher::finished was actually delivered.
        watcherReply: replies.some((l) => !/via poll fallback/.test(l)),
        echoes: logs.filter((l) => /M0 echo sent/.test(l)).length,
        counters
    };
}

async function runCase(browserType, name) {
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        const context = await browser.newContext({ ignoreHTTPSErrors: true });
        const page = await context.newPage();
        const logs = [];
        page.on("console", (msg) => logs.push(msg.text()));
        page.on("pageerror", (err) => logs.push("PAGEERROR " + err.message));

        // Armed before Qt boots, from the page's own query string, so every posted event the
        // client ever makes is starved. The client still has to connect, and it does, because
        // nothing on the socket path depends on the posted-event queue.
        await page.goto(`${pageUrl("ws", WS_PORT)}&starve=load`,
                        { waitUntil: "load", timeout: 60000 });

        const settled = await waitFor(() => {
            const now = analyzePump(logs);
            return now.connected && now.prop && now.signal && now.model
                && (now.watcherReply || now.pollReply);
        }, CASE_TIMEOUT_MS);

        // Give a late watcher a fair chance: if the poll resolved first, keep watching for a
        // while longer before concluding the queued signal never arrived at all.
        if (settled && !analyzePump(logs).watcherReply) {
            await waitFor(() => analyzePump(logs).watcherReply, 6000);
        }

        return { name, ...analyzePump(logs), logs };
    } finally {
        await browser.close();
    }
}

function verdict(result) {
    // Either verdict is only meaningful if the page really was starved and really did stay
    // connected with data flowing. Without that, "the watcher did not fire" could just mean
    // the client never got anywhere, and "the watcher fired" could mean the shim never
    // dropped anything.
    if (!result.starving || !result.dropped) {
        return { ok: false, why: "the wakeup was never actually starved" };
    }
    if (!result.connected || !result.prop || !result.signal || !result.model) {
        return { ok: false, why: "the starved page never got far enough to measure" };
    }
    if (expectation === "stall") {
        if (result.watcherReply) {
            return { ok: false, why: "the watcher fired, so this build drains posted events" };
        }
        // Without the poll line there is no evidence the reply ever arrived, and a reply that
        // never arrived is a different failure from a reply that arrived unannounced. It does
        // happen: with every posted event starved, a client is a degraded thing, and Firefox
        // sometimes does not complete the round trip inside the window at all. That is not
        // the stall under test, so say so and try again rather than score it either way.
        return result.pollReply
            ? { ok: true, why: "the reply arrived and resolved; the queued completion never came" }
            : { ok: false, retry: true, why: "no reply arrived at all, so nothing to judge" };
    }
    return result.watcherReply
        ? { ok: true, why: "the watcher fired even though the wakeup was starved" }
        : { ok: false, why: "the watcher still never fired" };
}

async function main() {
    console.log(`M0 pump-starvation repro: expecting "${expectation}", headless=${headless}`);
    renderShell();
    const staticServer = await startStaticServer({
        port: STATIC_PORT,
        extraScripts: [{ route: "/__m0-pump-starve.js", file: "pump-starve.js" }]
    });
    await startEdge();

    const candidates = [[chromium, "chromium"], [firefox, "firefox"], [webkit, "webkit"]];
    const browsers = [];
    for (const [browserType, browserName] of candidates) {
        try {
            const probe = await browserType.launch(launchOptions(browserType));
            await probe.close();
            browsers.push([browserType, browserName]);
        } catch {
            console.log(`  skipping ${browserName}: runtime not installed`);
        }
    }

    const results = [];
    try {
        for (const [browserType, browserName] of browsers) {
            const name = `${browserName}-starved`;
            let result = null;
            let call = null;
            for (let attempt = 1; attempt <= 3; ++attempt) {
                process.stdout.write(`\n=== case ${name} (attempt ${attempt}) ===\n`);
                result = await runCase(browserType, name);
                call = verdict(result);
                if (!call.retry) {
                    break;
                }
                console.log(`    inconclusive: ${call.why}`);
            }
            results.push({ ...result, ...call });
            console.log(
                `    ${call.ok ? "PASS" : "FAIL"} starved=${result.starving} ` +
                    `dropped=${result.dropped} ` +
                    `connected=${result.connected} prop=${result.prop} ` +
                    `signal=${result.signal} model=${result.model} ` +
                    `echoes=${result.echoes} pollReply=${result.pollReply} ` +
                    `watcherReply=${result.watcherReply}`);
            console.log(`    ${call.why}`);
        }
    } finally {
        await stopEdge();
        staticServer.close();
    }

    const failed = results.filter((r) => !r.ok);
    console.log("\n============== PUMP STARVATION SUMMARY ==============");
    for (const r of results) {
        console.log(`  ${r.ok ? "PASS" : "FAIL"}  ${r.name}`);
    }
    console.log("=====================================================");
    process.exit(failed.length > 0 ? 1 : 0);
}

main().catch((err) => {
    console.error("pump repro crashed:", err);
    stopEdge().finally(() => process.exit(2));
});
