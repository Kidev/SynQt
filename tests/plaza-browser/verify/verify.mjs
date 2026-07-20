// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The 3D plaza in a real browser, as two people in two tabs of one browser.
//
// The client draws to a canvas, so it is read through the telemetry run-plaza-browser.sh adds
// to its copy of the example: a "PLAZA {...}" console line four times a second saying where
// the walker is, what the keys ask for, who else it sees, and how many of the snapshots it
// kept are not numbers. Checks, in order:
//   1. each tab signs in through the development picker and sees the other person by name;
//   2. no snapshot a tab kept is anything but a number. The edge republishes its model every
//      step and a mirror receives rows before their values, so a client that read a row too
//      early kept `undefined` as a position;
//   3. keys reach the walker after a click, and again after a second click elsewhere. Qt's
//      WebAssembly port reads keys only once it has given its window the page's focus, which
//      it does when the focused item changes, and a click takes that focus back;
//   4. the walker moves at walking speed, and where Alice's own physics puts her is where the
//      edge says she is, as Bob's tab draws her.

import { chromium } from "playwright";

const base = process.env.PLAZA_URL || "http://127.0.0.1:8097";
const speed = 350;             // web/edge/Edge.qml and client/app/Main.qml agree on it
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let failures = 0;

function check(ok, what) {
    console.log(`  ${ok ? "ok  " : "FAIL"}  ${what}`);
    if (!ok) {
        failures += 1;
    }
}

// Headless Chromium has no GPU here. SwiftShader gives it the WebGL context Qt Quick 3D needs.
const browser = await chromium.launch({
    args: ["--no-sandbox", "--disable-dev-shm-usage", "--use-angle=swiftshader",
           "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"],
});
const context = await browser.newContext({ viewport: { width: 1100, height: 700 } });

async function person(index, name) {
    const page = await context.newPage();
    const state = { name, page, last: null, errors: [] };
    page.on("console", (message) => {
        const text = message.text();
        const at = text.indexOf("PLAZA ");
        if (at >= 0) {
            state.last = JSON.parse(text.slice(at + 6));
        } else if (message.type() === "error" || /Error:|TypeError/.test(text)) {
            state.errors.push(text);
        }
    });
    page.on("pageerror", (error) => state.errors.push(error.message));
    await page.goto(`${base}/synqt/dev/identity`);
    // This tab's own session, so two tabs of one browser are two people.
    await page.check(`#this-tab-only-identity-${index}`);
    await page.click(`button[data-identity="${name}@example.com"]`);
    return state;
}

async function until(state, test, what, ms = 60000) {
    const deadline = Date.now() + ms;
    while (Date.now() < deadline) {
        if (state.last && test(state.last)) {
            return state.last;
        }
        await sleep(250);
    }
    console.log(`    last line from ${state.name}: ${JSON.stringify(state.last)}`);
    return null;
}

// A tab in the background renders nothing, and the physics steps with rendering, so the tab
// being measured is always the one in front.
async function front(state) {
    await state.page.bringToFront();
    await sleep(1000);
}

async function hold(state, key, ms) {
    await state.page.keyboard.down(key);
    await sleep(ms);
    await state.page.keyboard.up(key);
    await sleep(600);
}

try {
    console.log("1. two people");
    const alice = await person(0, "alice");
    const aliceIn = await until(alice, (line) => line.user, "alice signs in", 120000);
    check(aliceIn !== null, "Alice's tab boots and holds `user`");
    const bob = await person(1, "bob");
    const bobIn = await until(bob, (line) => line.user, "bob signs in", 120000);
    check(bobIn !== null, "Bob's tab boots and holds `user`");

    await front(bob);
    const bobSees = await until(bob, (line) => line.others.some((o) => o.name === "alice"));
    check(bobSees !== null, "Bob sees Alice, by name");
    await front(alice);
    const aliceSees = await until(alice, (line) => line.others.some((o) => o.name === "bob"));
    check(aliceSees !== null, "Alice sees Bob, by name");

    console.log("2. only numbers kept");
    check(alice.last && alice.last.notANumber === 0 && bob.last && bob.last.notANumber === 0,
          `no snapshot kept as anything but a number (alice ${alice.last?.notANumber}, `
          + `bob ${bob.last?.notANumber})`);
    check(alice.last && alice.last.others.every((o) => typeof o.name === "string"
                                                     && o.name.length > 0
                                                     && typeof o.hue === "number"),
          "every walker drawn has a name and a colour");

    console.log("3. keys, after one click and after another");
    await alice.page.mouse.click(550, 600);
    await sleep(300);
    const beforeWalk = alice.last;
    await hold(alice, "w", 1500);
    const afterWalk = alice.last;
    const walked = Math.hypot(afterWalk.x - beforeWalk.x, afterWalk.z - beforeWalk.z);
    check(walked > 100, `W after a click walks Alice (${walked.toFixed(0)} units)`);
    // A wall or a person stops her short. Nothing makes her faster than walking.
    check(walked <= speed * 2.1 * 1.2,
          `no faster than walking (${walked.toFixed(0)} in about 2.1 s)`);

    await alice.page.mouse.click(250, 450);
    await sleep(300);
    const beforeSide = alice.last;
    await hold(alice, "d", 1000);
    const afterSide = alice.last;
    const stepped = Math.hypot(afterSide.x - beforeSide.x, afterSide.z - beforeSide.z);
    check(stepped > 50, `D after a second click still moves her (${stepped.toFixed(0)} units)`);

    console.log("4. the edge agrees");
    await sleep(1000);
    const own = alice.last;
    await front(bob);
    const seen = await until(bob, (line) => line.others.some((o) => o.name === "alice"));
    const drawn = seen && seen.others.find((o) => o.name === "alice");
    const apart = drawn ? Math.hypot(drawn.x - own.x, drawn.z - own.z) : Infinity;
    check(apart < 60, `Bob draws Alice where her own physics put her (${apart.toFixed(0)} apart)`);

    for (const state of [alice, bob]) {
        check(state.errors.length === 0, `no script errors in ${state.name}'s tab`
              + (state.errors.length ? `: ${state.errors.slice(0, 3).join(" | ")}` : ""));
    }
} catch (error) {
    console.log(`FAIL  ${error.stack || error}`);
    failures += 1;
} finally {
    await browser.close();
}

console.log(failures === 0 ? "PLAZA BROWSER: PASS" : `PLAZA BROWSER: FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
