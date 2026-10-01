// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The development scope picker, in a real browser.
//
// One claim needs a browser and cannot be made anywhere else. Two tabs of one browser
// context share one cookie jar, because RFC 6265 scopes a cookie to a host and not a port,
// and "the second sign-in did not become the first" is a statement about that jar.
// tests/webedge proves the edge sets two differently-named cookies. Only this proves a
// browser then keeps two sessions and hands each tab its own.
//
// Which bundle came back is how a tab is asked who it is. The session cookie is httpOnly,
// so a page cannot read it. The edge answers `/` with the bundle mapped to the caller's
// scope, and three directories of one-line HTML make that answer readable.

import { chromium, firefox } from "playwright";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const work = process.env.IDENTITY_PICKER_WORK;
if (!work) {
    console.error("IDENTITY_PICKER_WORK is not set; run this through run-identity-picker.sh");
    process.exit(2);
}
const project = path.join(work, "project");
const edgeBin = path.join(work, "native/web");
const bundles = path.join(project, "bundles");
const PICKER = "/synqt/dev/identity";

const headless = process.env.IDENTITY_PICKER_HEADLESS === "1" ? true : !process.env.DISPLAY;

function firstLine(err) {
    const line = String(err.message)
        .split("\n")
        .map((text) => text.replace(/[\u2500-\u257f]/g, "").trim())
        .find((text) => /[a-z]/i.test(text) && !/^browserType\.launch:?$/.test(text));
    return line || "the runtime would not launch and gave no reason";
}

// The edge under `synqt dev --identity-picker`, with the arguments `synqt dev` composes:
// one bundle per scope, the picker, and the named people it read out of `.dev-identities`.
// Started here rather than through the CLI so the harness holds the process and the port.
function startEdge() {
    return new Promise((resolve, reject) => {
        const proc = spawn(edgeBin, [
            "--port=0",
            "--dev",
            "--identity-picker",
            "--qml-dir", path.join(project, "generated"),
            "--bundle", `anonymous=${path.join(bundles, "anonymous")}`,
            "--bundle", `user=${path.join(bundles, "user")}`,
            "--bundle", `moderator=${path.join(bundles, "moderator")}`,
            // What `synqt dev` would pass after reading `.dev-identities`: the entry that
            // survived, and the sentence about the one that did not.
            "--dev-identity=moderator=alice@example.com",
            "--dev-identity-problem=.dev-identities entry 2 (nobody@example.com) names "
                + "scope 'wizard', which this project does not declare",
        ], {
            cwd: project,
            stdio: ["ignore", "pipe", "pipe"],
            env: { QT_QPA_PLATFORM: "offscreen", ...process.env },
        });
        let done = false;
        const onData = (chunk) => {
            const text = chunk.toString();
            process.stdout.write("[web] " + text);
            const match = text.match(/http:\/\/127\.0\.0\.1:(\d+)/);
            if (!done && match) {
                done = true;
                resolve({ proc, port: Number(match[1]) });
            }
        };
        proc.stdout.on("data", onData);
        proc.stderr.on("data", onData);
        proc.on("exit", (code) => {
            if (!done) reject(new Error("the edge exited early: " + code));
        });
        setTimeout(() => {
            if (!done) reject(new Error("the edge did not report listening"));
        }, 15000);
    });
}

/// Sign in through the picker in this tab, ticking "this tab only", and come back with the
/// page the edge redirected to. Everything about it is what a person does. The form is the
/// picker's own, and the browser carries the cookie jar and follows the redirect.
async function signInPerTab(page, origin, scope) {
    await page.goto(origin + PICKER, { waitUntil: "load", timeout: 30000 });
    await page.check(`#this-tab-only-${scope}`);
    await Promise.all([
        page.waitForNavigation({ waitUntil: "load", timeout: 30000 }),
        page.click(`[data-scope="${scope}"]`),
    ]);
}

async function whoAmI(page) {
    return (await page.textContent("#who"))?.trim();
}

async function runCase(browserType, name) {
    const { proc: edge, port } = await startEdge();
    const origin = `http://127.0.0.1:${port}`;
    const browser = await browserType.launch({ headless });
    try {
        // One context for all three tabs. A mechanism that only works across contexts
        // proves nothing. Separate contexts have separate jars, which is the very thing
        // per-tab sessions exist to work without.
        const context = await browser.newContext();

        // 1. The picker draws the project's own vocabulary, and the named person from
        //    `.dev-identities` beside it, and says what it could not use.
        const first = await context.newPage();
        await first.goto(origin + PICKER, { waitUntil: "load", timeout: 30000 });
        const page = await first.content();
        for (const scope of ["anonymous", "user", "moderator"]) {
            if (!page.includes(`data-scope="${scope}"`)) {
                throw new Error(`the picker did not offer '${scope}'`);
            }
        }
        if (page.includes("data-scope=\"admin\"")) {
            throw new Error("the picker offered a scope this project never declared");
        }
        if (!page.includes("alice@example.com")) {
            throw new Error("the picker did not offer the named identity");
        }
        if (!page.includes("wizard")) {
            throw new Error("the picker did not report the entry it could not use");
        }
        console.log("  the picker offers this project's scopes, and the named person");

        // 2. Tab one becomes a moderator, for itself alone.
        await signInPerTab(first, origin, "moderator");
        if (!first.url().includes("?s=")) {
            throw new Error(`a per-tab sign-in did not carry a nonce: ${first.url()}`);
        }
        if (await whoAmI(first) !== "moderator") {
            throw new Error(`tab one is ${await whoAmI(first)}, not a moderator`);
        }
        console.log("  tab one holds a moderator session of its own");

        // 3. Tab two becomes a user, in the same jar. This is the whole suite: before
        //    per-tab sessions existed, this sign-in overwrote the one above.
        const second = await context.newPage();
        await signInPerTab(second, origin, "user");
        if (await whoAmI(second) !== "user") {
            throw new Error(`tab two is ${await whoAmI(second)}, not a user`);
        }
        await first.reload({ waitUntil: "load", timeout: 30000 });
        if (await whoAmI(first) !== "moderator") {
            throw new Error(`tab one became ${await whoAmI(first)} when tab two signed in`);
        }
        console.log("  tab two holds a user session, and tab one is still a moderator");

        // 4. And a tab that carries no nonce is nobody, even though the jar holds two
        //    sessions. A per-tab session is reachable only from the tab that asked for it.
        const third = await context.newPage();
        await third.goto(origin + "/", { waitUntil: "load", timeout: 30000 });
        if (await whoAmI(third) !== "anonymous") {
            throw new Error(`a tab with no nonce is ${await whoAmI(third)}, not anonymous`);
        }
        console.log("  a third tab, with no nonce, is anonymous");

        // 5. The shared mode still works, and is what a tab with no nonce picks up: this is
        //    the ordinary sign-in the picker replaced, and it must not have been lost to
        //    the per-tab machinery.
        await third.goto(origin + PICKER, { waitUntil: "load", timeout: 30000 });
        await Promise.all([
            third.waitForResponse((response) => response.url().includes(PICKER)
                                                && response.request().method() === "POST",
                                  { timeout: 30000 }),
            third.click('[data-scope="user"]'),
        ]);
        await third.goto(origin + "/", { waitUntil: "load", timeout: 30000 });
        if (await whoAmI(third) !== "user") {
            throw new Error(`a shared sign-in left the tab as ${await whoAmI(third)}`);
        }
        if (await whoAmI(first) !== "moderator") {
            throw new Error("a shared sign-in overwrote a per-tab session");
        }
        console.log("  a shared sign-in works too, and leaves the per-tab sessions alone");

        return { name, pass: true };
    } catch (err) {
        return { name, pass: false, error: err.message };
    } finally {
        await browser.close();
        edge.kill("SIGKILL");
    }
}

async function main() {
    const candidates = [[chromium, "chromium"], [firefox, "firefox"]];
    const engines = [];
    const versions = {};
    for (const [browserType, browserName] of candidates) {
        let probe = null;
        try {
            probe = await browserType.launch({ headless });
        } catch (err) {
            console.log(`  skipping ${browserName}: ${firstLine(err)}`);
            continue;
        }
        versions[browserName] = probe.version();
        engines.push([browserType, browserName]);
        await probe.close();
    }
    if (engines.length === 0) {
        // The toolchain is missing, so nothing was tested. Announced and given its
        // own exit code so the runner can say PARTIAL out loud. A skip that prints nothing
        // is indistinguishable from a pass, and this suite's whole subject is a browser.
        console.log("IDENTITY PICKER: no browser engine could be launched");
        process.exit(3);
    }
    const engineList = engines.map(([, n]) => `${n} ${versions[n]}`).join(", ");
    console.log(`identity-picker verify: headless=${headless}  engines: ${engineList}`);

    const results = [];
    for (const [browserType, browserName] of engines) {
        console.log(`\n=== case ${browserName} ===`);
        const result = await runCase(browserType, browserName);
        results.push(result);
        console.log(`    ${result.pass ? "PASS" : "FAIL"} ${result.name}`
                    + (result.error ? `: ${result.error}` : ""));
    }

    console.log("\n============== IDENTITY PICKER SUMMARY ==============");
    console.log(`  engines: ${engineList}`);
    for (const result of results) {
        console.log(`  ${result.pass ? "PASS" : "FAIL"}  ${result.name}`);
    }
    console.log("====================================================");
    if (results.some((result) => !result.pass)) {
        console.log("IDENTITY PICKER: FAIL");
        process.exit(1);
    }
    console.log("IDENTITY PICKER: PASS");
    process.exit(0);
}

main().catch((err) => {
    console.error("\nIDENTITY PICKER: FAIL --", err.message);
    process.exit(1);
});
