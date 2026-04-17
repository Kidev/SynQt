// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The monitoring console, in a real browser, against a real monitor.
//
// Everything under test here is invisible to a compiler. The delivery gate is an HTTP
// answer, the sign-in is a form somebody fills in, the console is a Qt Quick scene that
// either draws or does not, and the proof that it connected is a row appearing in the
// monitor's own record. A build says none of that. This repository has shipped a client
// whose root was not a window, a delegate whose roles collided with Item, and a model
// binding that made a file unloadable, and every one of them compiled cleanly.
//
// The project it drives is written by the scaffolder at run time (make-project.py), so
// what loads here is what `synqt add entity --type monitor` produces today.

import { chromium, firefox, webkit } from "playwright";
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../../..");
const work = process.env.MONITOR_CONSOLE_WORK;
if (!work) {
    console.error("MONITOR_CONSOLE_WORK is not set; run this through run-monitor-console.sh");
    process.exit(2);
}
const project = path.join(work, "project");
const monitorBin = path.join(work, "native/ops");
const edgeBin = path.join(work, "native/web");
const consoleBundle = path.join(work, "bundle-console");
const signinBundle = path.join(project, "monitor/ops/signin");
// The monitor's own JSONL export, which is how this harness reads the record
// without reaching into SQLite: `export.jsonl` is on in this project for that
// reason and the exporter is under test in tests/monitor.
const exportFile = path.join(project, "build/ops/state/events.jsonl");

const OPERATOR = "alice";
const PASSWORD = "correct-horse-battery";

// Where to write the console's portrait, when somebody wants one. The docs carry a picture
// of the console (docs/monitoring.md), and this is the only place in the repository where a
// real console is running against a real monitor with real events in it, so this is where
// the picture comes from. Off unless asked for. The suite's job is to pass or fail, and a
// run that also writes a file into the source tree should be a run somebody asked for.
//
//   MONITOR_CONSOLE_SHOT=docs/assets/monitoring-console.png
//
// Written from the first engine that gets that far, so the picture is reproducible rather
// than whichever of three engines finished last.
const shotPath = process.env.MONITOR_CONSOLE_SHOT
    ? path.resolve(repoRoot, process.env.MONITOR_CONSOLE_SHOT)
    : null;
let shotTaken = false;

const headless = process.env.MONITOR_CONSOLE_HEADLESS === "1" ? true : !process.env.DISPLAY;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function firstLine(err) {
    const line = String(err.message)
        .split("\n")
        .map((text) => text.replace(/[\u2500-\u257f]/g, "").trim())
        .find((text) => /[a-z]/i.test(text) && !/^browserType\.launch:?$/.test(text));
    return line || "the runtime would not launch and gave no reason";
}

function launchOptions(browserType) {
    const options = { headless, args: [] };
    if (browserType === chromium) {
        // Qt Quick has no software fallback on WebAssembly: with no WebGL context the
        // scene graph cannot be created and the client aborts. Chromium is the one engine
        // that takes its rasteriser as an argument and brings SwiftShader with it.
        options.args.push("--use-gl=angle", "--use-angle=swiftshader",
                          "--enable-unsafe-swiftshader");
    }
    return options;
}

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

// The monitor, on an ephemeral port, pointed at the two bundles the delivery gate chooses
// between. Its store and its JSONL export are fresh per case, so one case never reads what
// an earlier one wrote.
function startMonitor(caseName) {
    const store = path.join(work, `store-${caseName}.db`);
    for (const stale of [store, exportFile]) {
        fs.rmSync(stale, { force: true });
    }
    return new Promise((resolve, reject) => {
        const proc = spawn(monitorBin, [
            "--port=0",
            "--store", store,
            "--qml-dir", path.join(project, "generated"),
            "--topology", path.join(project, "build/ops/topology.json"),
            "--bundle", `anonymous=${signinBundle}`,
            "--bundle", `operator=${consoleBundle}`,
        ], {
            cwd: project,
            stdio: ["ignore", "pipe", "pipe"],
            env: { QT_QPA_PLATFORM: "offscreen", ...process.env },
        });
        let done = false;
        const onData = (chunk) => {
            const text = chunk.toString();
            process.stdout.write("[ops] " + text);
            const match = text.match(/http:\/\/127\.0\.0\.1:(\d+)/);
            if (!done && match) {
                done = true;
                resolve({ proc, port: Number(match[1]) });
            }
        };
        proc.stdout.on("data", onData);
        proc.stderr.on("data", onData);
        proc.on("exit", (code) => {
            if (!done) reject(new Error("the monitor exited early: " + code));
        });
        setTimeout(() => {
            if (!done) reject(new Error("the monitor did not report listening"));
        }, 15000);
    });
}

// The reporting entity, so the console has an entity other than the monitor to show. It is
// an ordinary web edge on an ordinary mesh link. Mutual TLS against the project's own CA,
// like every other link in a SynQt system.
function startEdge() {
    const proc = spawn(edgeBin, [
        "--port=0",
        "--bundle", path.join(work, "wasm"),
        "--qml-dir", path.join(project, "generated"),
        "--topology", path.join(project, "build/web/topology.json"),
    ], {
        cwd: project,
        stdio: ["ignore", "pipe", "pipe"],
        env: { QT_QPA_PLATFORM: "offscreen", ...process.env },
    });
    proc.stdout.on("data", (c) => process.stdout.write("[web] " + c.toString()));
    proc.stderr.on("data", (c) => process.stdout.write("[web] " + c.toString()));
    return proc;
}

function exportedEvents() {
    if (!fs.existsSync(exportFile)) {
        return [];
    }
    return fs.readFileSync(exportFile, "utf8")
        .split("\n")
        .filter((line) => line.trim().length > 0)
        .map((line) => {
            try {
                return JSON.parse(line);
            } catch {
                return null;
            }
        })
        .filter(Boolean);
}

async function waitFor(predicate, timeoutMs, label) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        if (await predicate()) return true;
        await sleep(250);
    }
    throw new Error("timed out waiting for: " + label);
}

// Whether the console drew anything, measured on a screenshot rather than on the canvas.
//
// Reading the WebGL canvas directly is the obvious way and it does not work: Qt creates its
// context without `preserveDrawingBuffer`, so by the time anything here can call readPixels
// the drawing buffer has been composited and cleared, and every frame reads as blank. A
// screenshot comes from the composited output, which is what a person looking at the page
// sees.
//
// The measure is the size of the PNG. A flat fill of one colour compresses to a few
// kilobytes at this resolution whatever colour it is. A console with a counters strip, a
// filter row and a table of events does not come close to that. This is the assertion that
// catches the failure this repository keeps meeting: QML that builds green, logs nothing,
// and renders an empty window.
const DREW_BYTES = 12000;

async function screenshotSize(page) {
    return (await page.screenshot()).length;
}

// Every file of the console, asked for by a caller whose session holds only `anonymous`.
//
// 404 rather than 403, because a bundle outside the caller's scope is not addressable at
// all, so probing for one tells a visitor nothing about what this deployment is running.
// A 403 would answer the question it is refusing to answer.
async function theConsoleBundleIsNotAddressableToAnAnonymousCaller(page, origin) {
    for (const probe of ["/ops-console.js", "/ops-console.wasm", "/qtloader.js"]) {
        const answer = await page.request.get(origin + probe);
        if (answer.status() !== 404) {
            throw new Error(`${probe} answered ${answer.status()} to an anonymous caller; `
                            + "the console bundle must not be addressable");
        }
    }
}

async function runCase(browserType, name) {
    const logs = [];
    const { proc: monitor, port } = await startMonitor(name);
    const edge = startEdge();
    const origin = `http://127.0.0.1:${port}`;
    const browser = await browserType.launch(launchOptions(browserType));
    try {
        // A stated viewport rather than the default, because one of the things this
        // suite produces is a picture for the docs and a picture wants a known size. It is
        // also the better test. A console laid out for whatever window Playwright
        // happened to open is a console nobody has looked at.
        const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
        const page = await context.newPage();
        page.on("console", (msg) => {
            logs.push(msg.text());
            if (process.env.VERBOSE) console.log(`  [${name}] ${msg.text()}`);
        });
        page.on("pageerror", (e) => logs.push("PAGEERROR " + e.message));

        // 1. An anonymous visitor is handed the gate, and the console is not there to be
        //    found. 404 and not 403: a bundle outside the caller's scope is not addressable
        //    at all, so probing for one tells a visitor nothing about what exists.
        console.log("  loading the monitor as an anonymous visitor");
        await page.goto(origin + "/", { waitUntil: "load", timeout: 30000 });
        const gate = await page.content();
        if (!gate.includes("sign in") && !gate.includes("operator")) {
            throw new Error("an anonymous visitor was not handed the sign-in gate");
        }
        await theConsoleBundleIsNotAddressableToAnAnonymousCaller(page, origin);
        console.log("  the console bundle is not addressable without an operator session");

        // 2. The gate is a real page with a real form, and its inline script has to survive
        //    the edge's strict Content-Security-Policy. Filling it in is the only thing that
        //    says whether it does. A blocked script leaves a form that looks right and does
        //    nothing when submitted.
        const refused = [];
        page.on("console", (msg) => {
            if (/Content Security Policy|Refused to execute/i.test(msg.text())) {
                refused.push(msg.text());
            }
        });
        console.log("  filling in the sign-in form");
        await page.fill('input[name="name"]', OPERATOR);
        await page.fill('input[name="password"]', "wrong-password");
        await page.click('button[type="submit"]');
        await waitFor(async () => (await page.textContent("#said"))?.includes("did not work"),
                      10000, "the gate to refuse a wrong password");
        if (refused.length > 0) {
            throw new Error("the gate's inline script was refused by the CSP: " + refused[0]);
        }
        console.log("  a wrong password is refused, and says one thing for every failure");

        await page.fill('input[name="password"]', PASSWORD);
        await Promise.all([
            page.waitForNavigation({ waitUntil: "load", timeout: 30000 }),
            page.click('button[type="submit"]'),
        ]);
        console.log("  signed in; the same URL is now a different bundle");

        // 3. The console is what came back, and it drew.
        await waitFor(async () => (await page.locator("canvas").count()) > 0, 60000,
                      "the console to put a canvas on the page");
        await waitFor(async () => (await screenshotSize(page)) > DREW_BYTES, 60000,
                      "the console to draw something rather than an empty window");
        console.log(`  the console rendered (${await screenshotSize(page)} byte screenshot)`);

        // 4. And it connected. The console's link is a browser upgrade like any other, so
        //    the monitor records accepting one, and nothing was added to the console's QML
        //    to say so. The proof that it reached the monitor is in the monitor.
        await waitFor(async () => exportedEvents().some(
            (event) => event.entity === "ops" && event.message === "upgrade accepted"),
                      60000, "the monitor to accept the console's upgrade");
        console.log("  the console's wss link came up and the monitor recorded it");

        // 5. The operator sign-in is in the record too, both halves of it: a monitor that
        //    cannot tell you somebody failed to sign in is missing the event an operator
        //    most wants to find.
        const events = exportedEvents();
        const accepted = events.filter((e) => e.message === "operator signed in");
        const rejected = events.filter((e) => e.message === "operator sign-in refused");
        if (accepted.length !== 1 || rejected.length !== 1) {
            throw new Error(`the record holds ${accepted.length} sign-in(s) and `
                            + `${rejected.length} refusal(s); expected one of each`);
        }
        if (JSON.stringify(events).includes(PASSWORD)) {
            throw new Error("the operator's password appears in the record");
        }
        console.log("  both halves of the gate are in the record, and the password is not");

        // 6. A second entity's events reached the monitor over the mesh, which is the whole
        //    fan-in. The console is showing a system and not only itself.
        await waitFor(async () => exportedEvents().some((event) => event.entity === "web"),
                      60000, "the web edge to report through the mesh");
        console.log("  the web edge's events reached the monitor over mutual TLS");

        // 7. The picture for the docs, taken here and not earlier. By this line the console
        //    is showing a system rather than itself, because a second entity's events have
        //    crossed the mesh into the table. A screenshot taken at step 3 would be a
        //    correct console with one row in it, which teaches a reader nothing about what
        //    the thing is for.
        if (shotPath && !shotTaken) {
            await sleep(1500);  // let the table settle on the frame it gained
            fs.mkdirSync(path.dirname(shotPath), { recursive: true });
            // Clipped rather than shrunk. The window stays the 1440x900 one the assertions
            // above ran against, because that is a window somebody would open;
            // what the docs want is the part of it with the console in it, and a test run
            // with a viewport chosen to flatter a screenshot is a test run about the
            // screenshot.
            await page.screenshot({
                path: shotPath,
                clip: { x: 0, y: 0, width: 1440, height: 620 },
            });
            shotTaken = true;
            console.log(`  wrote ${shotPath}`);
        }

        return { name, pass: true, logs };
    } catch (err) {
        return { name, pass: false, error: err.message, logs };
    } finally {
        await browser.close();
        edge.kill("SIGKILL");
        monitor.kill("SIGKILL");
    }
}

function dumpEvidence(name, logs) {
    const limit = 120;
    console.log(`    --- ${logs.length} line(s) seen by ${name}`
                + (logs.length > limit ? `, first ${limit} shown` : "") + " ---");
    for (const line of logs.slice(0, limit)) {
        console.log(`      ${line}`);
    }
    const events = exportedEvents();
    console.log(`    --- ${events.length} exported event(s), last 20 ---`);
    for (const event of events.slice(-20)) {
        console.log(`      ${event.entity} ${event.severity} ${event.message}`);
    }
}

async function main() {
    const candidates = [[chromium, "chromium"], [firefox, "firefox"], [webkit, "webkit"]];
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
            const renderer = await graphicsRenderer(probe);
            if (!renderer) {
                unproven.push([browserName, "no WebGL context, so Qt Quick cannot start"]);
                console.log(`  skipping ${browserName}: no WebGL context, so the Qt Quick `
                            + "console cannot start (install Mesa's software rasteriser)");
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
    const engineList = engines.map(([, n]) => `${n} ${versions[n]}`).join(", ");
    console.log(`monitor-console verify: headless=${headless}  engines: ${engineList}`);

    const results = [];
    for (const [browserType, browserName] of engines) {
        console.log(`\n=== case ${browserName} ===`);
        const result = await runCase(browserType, browserName);
        results.push(result);
        console.log(`    ${result.pass ? "PASS" : "FAIL"} ${result.name}`
                    + (result.error ? `: ${result.error}` : ""));
        if (!result.pass) {
            dumpEvidence(browserName, result.logs);
        }
    }

    console.log("\n============== MONITOR CONSOLE SUMMARY ==============");
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
        console.log(`MONITOR CONSOLE: FAIL (${failed.length} failing engine(s))`);
        process.exit(1);
    }
    console.log("MONITOR CONSOLE: PASS");
    process.exit(0);
}

main().catch((err) => {
    console.error("\nMONITOR CONSOLE: FAIL --", err.message);
    process.exit(1);
});
