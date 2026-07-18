// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M6 browser end-to-end: the WASM counter client runs in a real browser against the
// real web edge, and two tabs stay in sync. The QML renders to a canvas, so the client
// surfaces "M6 state=... counter=..." to the console (Main.qml telemetry) and this
// harness asserts on those sentinels and drives the "+" button by clicking the canvas.

import { chromium } from "playwright";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const edgeBin = path.join(repoRoot, "build/m6-app-desktop/counter-edge");
const bundleDir = path.join(repoRoot, "build/m6-app-wasm");
const counterQml = path.join(repoRoot, "tests/m6-client/web/Counter.qml");

const headless = process.env.M6_HEADLESS === "1" ? true : !process.env.DISPLAY;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function startEdge() {
    return new Promise((resolve, reject) => {
        const proc = spawn(edgeBin, [
            `--bundle=${bundleDir}`, `--counter-qml=${counterQml}`, "--port=0",
        ], { stdio: ["ignore", "pipe", "pipe"] });
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

function openTab(context, name, logs) {
    return context.newPage().then((page) => {
        page.on("console", (msg) => {
            const text = msg.text();
            if (text.includes("M6 ")) {  // Qt prefixes QML console.log with "qml: "
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

async function main() {
    const { proc: edge, port } = await startEdge();
    const url = `http://127.0.0.1:${port}/`;
    const browser = await chromium.launch({
        headless,
        args: ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
    });
    try {
        const context = await browser.newContext();
        const logsA = [];
        const logsB = [];
        const tabA = await openTab(context, "tabA", logsA);
        const tabB = await openTab(context, "tabB", logsB);

        console.log("\n=== loading two tabs against the real edge ===");
        await tabA.goto(url, { waitUntil: "load", timeout: 60000 });
        await tabB.goto(url, { waitUntil: "load", timeout: 60000 });

        const connectedAt = (logs, value) =>
            logs.some((l) => l.includes("state=connected") && l.includes(`counter=${value}`));

        await waitFor(() => connectedAt(logsA, 0), 60000, "tab A connected, counter=0");
        await waitFor(() => connectedAt(logsB, 0), 60000, "tab B connected, counter=0");
        console.log("both tabs connected, counter=0");

        // Drive the "+" button on tab A's canvas. The window is 320x220, content centred,
        // so the "+" (right button in the row) sits right of centre, below the label.
        console.log("\n=== clicking + on tab A ===");
        const canvas = tabA.locator("canvas").first();
        const box = await canvas.boundingBox();
        await tabA.mouse.click(box.x + box.width / 2 + 24, box.y + box.height / 2 + 34);

        await waitFor(() => logsA.some((l) => l.includes("counter=1")), 20000, "tab A counter=1");
        // The other tab shares the same edge-owned counter: it must see the new value.
        await waitFor(() => logsB.some((l) => l.includes("counter=1")), 20000, "tab B counter=1 (sync)");
        console.log("both tabs show counter=1; two tabs stay in sync");

        console.log("\nM6 BROWSER: PASS");
    } finally {
        await browser.close();
        edge.kill("SIGKILL");
    }
}

main().catch((err) => {
    console.error("\nM6 BROWSER: FAIL --", err.message);
    process.exit(1);
});
