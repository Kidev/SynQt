// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The M0 matrix in real Safari.app, driven through safaridriver.
//
// Why this exists next to verify.mjs, which already runs WebKit: WebKit is Safari's engine, not
// Safari. Playwright's WebKit is a build of the engine with Playwright's own networking and no
// Apple TLS stack, and docs/browser-proofs.md has carried "Safari itself is not driven anywhere"
// as a known limit for exactly that reason. This closes it on the platform where Safari exists.
//
// Three constraints shape the whole file, all of them Safari's:
//
//   1. Playwright cannot drive Safari.app. safaridriver speaks W3C WebDriver, so the driver is
//      webdriver.mjs and not a browser automation library.
//   2. WebDriver standardises no logging endpoint, and Apple implements none, so the console
//      the other engines are judged by cannot be read here. The page keeps its own log
//      (console-tap.js, injected by the harness) and it is read back with execute/sync.
//   3. Safari has no headless mode and allows one session at a time, so this needs a logged-in
//      GUI session and runs its cases in sequence. It is not part of run-m0.sh for that reason.
//
// Usage: tests/m0-transport/verify/run-safari.sh   (or `node verify-safari.mjs` with a built
// client and `sudo safaridriver --enable` already done).

import {
    STATIC_PORT, WS_PORT, WSS_PORT,
    dumpEvidence, pageUrl, renderShell, startEdge, startStaticServer, stopEdge,
    waitFor, waitForAllPaths
} from "./harness.mjs";
import { Session, WebDriverError, pageLogs, startSafariDriver } from "./webdriver.mjs";

const DRIVER_PORT = Number(process.env.SAFARI_DRIVER_PORT || 4444);

// Safari refuses a self-signed certificate and, unlike every other engine here, offers no way to
// override it: WebDriver's acceptInsecureCerts is not implemented by safaridriver, and there is
// no command-line switch. The only way to run the wss case is to trust the harness certificate
// in the system keychain, which needs sudo and is therefore opt-in rather than assumed.
const wssRequested = process.env.SAFARI_WSS === "1";

async function withSession(base, name, run) {
    const session = await Session.create(base);
    try {
        return await run(session);
    } finally {
        await session.close();
    }
}

async function runCase(base, name, scheme, port) {
    return withSession(base, name, async (session) => {
        await session.navigate(pageUrl(scheme, port));
        const result = await waitForAllPaths(() => pageLogs(session));
        const logs = await pageLogs(session);
        return { name, ...result, logs };
    });
}

async function runReconnect(base, name, scheme, port) {
    return withSession(base, name, async (session) => {
        await session.navigate(pageUrl(scheme, port));

        const initial = await waitForAllPaths(() => pageLogs(session));
        if (!initial.pass) {
            const logs = await pageLogs(session);
            return { name, pass: false, stage: "initial", ...initial, logs };
        }

        // The mark is a line count, and the page's log only grows, so slicing from it is the
        // same "since this moment" the Playwright cases use.
        const mark = (await pageLogs(session)).length;
        await stopEdge();
        const sawDisconnect = await waitFor(
            async () => (await pageLogs(session)).slice(mark)
                .some((l) => /M0 state=disconnected/.test(l)),
            15000);

        await startEdge();
        const sawReconnect = await waitFor(
            async () => (await pageLogs(session)).slice(mark)
                .some((l) => /M0 state=connected/.test(l)),
            30000);
        const reconnectMark = (await pageLogs(session)).length;
        const sawFreshData = await waitFor(
            async () => (await pageLogs(session)).slice(reconnectMark)
                .some((l) => /M0 prop counter=/.test(l)),
            15000);

        return {
            name,
            pass: sawDisconnect && sawReconnect && sawFreshData,
            sawDisconnect,
            sawReconnect,
            sawFreshData,
            logs: await pageLogs(session)
        };
    });
}

async function main() {
    if (process.platform !== "darwin") {
        console.log("SAFARI GATE: SKIP (Safari.app exists only on macOS)");
        process.exit(0);
    }

    let driver;
    try {
        driver = await startSafariDriver(DRIVER_PORT);
    } catch (err) {
        // A machine without `safaridriver --enable` is a machine that has not opted in, not a
        // failing proof. Say exactly what to run; this is the one manual step.
        console.log(`SAFARI GATE: SKIP (${err.message})`);
        console.log("  enable it once with: sudo safaridriver --enable");
        process.exit(0);
    }

    renderShell();
    const staticServer = await startStaticServer({ port: STATIC_PORT });
    await startEdge();

    const version = await (async () => {
        try {
            return await withSession(driver.base, "probe", (session) =>
                session.execute("return navigator.userAgent;"));
        } catch {
            return "unknown";
        }
    })();
    console.log(`  Safari: ${version}`);

    const schemes = [["ws", WS_PORT]];
    if (wssRequested) {
        schemes.push(["wss", WSS_PORT]);
    }

    const results = [];
    try {
        for (const [scheme, port] of schemes) {
            const name = `safari-${scheme}`;
            process.stdout.write(`\n=== case ${name} ===\n`);
            const result = await runCase(driver.base, name, scheme, port);
            results.push(result);
            console.log(
                `    ${result.pass ? "PASS" : "FAIL"} connected=${result.connected} ` +
                    `prop=${result.prop} signal=${result.signal} reply=${result.reply} ` +
                    `model=${result.model} counters=[${result.counters.join(",")}] ` +
                    `rowsMax=${result.rowsMax}`);
            if (!result.pass) {
                dumpEvidence(result.logs);
            }
        }

        process.stdout.write("\n=== case safari-reconnect ===\n");
        const reconnect = await runReconnect(driver.base, "safari-reconnect", "ws", WS_PORT);
        results.push(reconnect);
        console.log(
            `    ${reconnect.pass ? "PASS" : "FAIL"} ` +
                `disconnect=${reconnect.sawDisconnect} reconnect=${reconnect.sawReconnect} ` +
                `freshData=${reconnect.sawFreshData}` +
                (reconnect.stage ? ` stage=${reconnect.stage}` : ""));
        if (!reconnect.pass) {
            dumpEvidence(reconnect.logs);
        }
    } finally {
        await stopEdge();
        staticServer.close();
        driver.proc.kill("SIGKILL");
    }

    const failed = results.filter((r) => !r.pass);
    console.log("\n================== SAFARI SUMMARY ==================");
    console.log(`  Safari: ${version}`);
    for (const r of results) {
        console.log(`  ${r.pass ? "PASS" : "FAIL"}  ${r.name}`);
    }
    if (!wssRequested) {
        console.log("  note  wss not run: Safari cannot be told to accept the harness's");
        console.log("        self-signed cert, so it must be trusted in the system keychain.");
        console.log("        Trust it, then re-run with SAFARI_WSS=1.");
    }
    console.log("===================================================");
    if (failed.length > 0) {
        console.log(`SAFARI GATE: NO-GO (${failed.length} failing case(s))`);
        process.exit(1);
    }
    console.log("SAFARI GATE: GO (all four QtRO paths and reconnect work in real Safari)");
    process.exit(0);
}

main().catch((err) => {
    const detail = err instanceof WebDriverError ? err.message : err;
    console.error("Safari verify crashed:", detail);
    stopEdge().finally(() => process.exit(2));
});
