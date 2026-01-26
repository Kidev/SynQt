// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The design editor, in a real browser, both ways it is reached.
//
// Case 1 is the editor over a project. `synqt design` on a copy of the gavel example, and a
// browser that adds a service, drags a connect point from it to the web edge, names the
// point and its contract, says a slot crosses it, reviews the change set and applies it. The
// verdict is on disk afterwards, in the two files it wrote. synqt.yaml, which carries what
// crosses the point, and the Source the owner hosts it with. Everything between the click and
// the file is what this covers, and
// none of it can be reached from Python: the canvas, the drag, the inspector, and the plan
// digest that ties Apply to the change set that was shown.
//
// Case 2 is the copy the documentation site publishes, which has no server behind it. The
// assets are served as static files under `Content-Security-Policy: default-src 'self'`, the
// policy that copy has to live under, and the page has to come up as a drawing board with
// Apply offering a download. Nothing may be fetched from another origin and nothing may be
// refused by the policy; the CLI has a server sending that header on every response, and the
// hosted copy has nobody, so this is where it is measured.
//
// Chromium only. What is being proven here is this page's own behaviour rather than
// an engine's. The transport proofs that are per-engine questions are tests/m0-transport and
// tests/m6-client. Exits 0 only if both cases pass.

import { chromium } from "playwright";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import fs from "node:fs";
import fsp from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "../..");
const gavel = path.join(repoRoot, "examples/gavel");
const assets = path.join(repoRoot, "tools/synqt/synqt/assets/design");
const python = process.env.PYTHON || "python3";

const headless = process.env.DESIGNER_HEADLESS === "1" ? true : !process.env.DISPLAY;

const failures = [];

function check(condition, what) {
    if (condition) {
        console.log(`  ok    ${what}`);
        return true;
    }
    console.log(`  FAIL  ${what}`);
    failures.push(what);
    return false;
}

// The editor over a project

// A copy, because this test applies a change set and the example is the documentation's.
// build/ and .synqt/ are left behind. The first is somebody's last build and the second
// holds the canvas layout, neither of which the editor reads to decide what to write.
async function copyProject() {
    const target = await fsp.mkdtemp(path.join(os.tmpdir(), "synqt-designer-"));
    const project = path.join(target, "gavel");
    await fsp.cp(gavel, project, {
        recursive: true,
        filter: (source) => !/(^|[\\/])(build|\.synqt|__pycache__)([\\/]|$)/.test(source),
    });
    return project;
}

function startEditor(project) {
    return new Promise((resolve, reject) => {
        // -u. The URL is printed and then the server blocks in serve_forever, so a buffered
        // stdout would hand this script the address only once the run was over.
        const proc = spawn(python, ["-u", "-m", "synqt", "design",
                                    "--project-dir", project, "--port", "0", "--no-open"],
                           { cwd: repoRoot, stdio: ["ignore", "pipe", "pipe"] });
        let settled = false;
        const onData = (chunk) => {
            const text = chunk.toString();
            process.stdout.write("[design] " + text);
            const found = text.match(/http:\/\/127\.0\.0\.1:\d+\/#token=\S+/);
            if (!settled && found) {
                settled = true;
                resolve({ proc, url: found[0] });
            }
        };
        proc.stdout.on("data", onData);
        proc.stderr.on("data", onData);
        proc.on("error", reject);
        proc.on("exit", (code) => {
            if (!settled) {
                reject(new Error(`synqt design exited before it served anything (${code})`));
            }
        });
        setTimeout(() => {
            if (!settled) {
                reject(new Error("synqt design printed no URL within 20s"));
            }
        }, 20000);
    });
}

// Wait for the page to say `prefix`, and report what it said instead when it never does.
// The hint line is where the editor puts every refusal, so a timeout here that only said
// "timed out" would be throwing away the one sentence that explains it.
async function waitForHint(page, prefix) {
    try {
        await page.waitForFunction(
            (wanted) => document.getElementById("hint").textContent.startsWith(wanted),
            prefix, { timeout: 30000 });
    } catch (error) {
        const said = await page.locator("#hint").textContent();
        throw new Error(`the page never said "${prefix}...". It said: ${said}`);
    }
}

// The centre of an entity's disc, in page coordinates. Taken from the circle rather than
// from the group, whose box includes the name and the kind written under it.
async function discCentre(page, name) {
    const box = await page.locator(`[data-entity="${name}"] .node__disc`).boundingBox();
    if (!box) {
        throw new Error(`no entity named '${name}' on the canvas`);
    }
    return { x: box.x + (box.width / 2), y: box.y + (box.height / 2) };
}

// Dragging is the only way an entity reaches the canvas, so it is the only way this test can
// put one there. `dragTo` is the HTML5 drag the page listens for, not a synthesised click.
async function dropEntity(page, label, at) {
    await page.locator(".palette__item", { hasText: label })
        .dragTo(page.locator("#canvas"), { targetPosition: at });
}

// A file is read-only until it is unlocked, which is the point of the lock: the pane holds
// the entity's own code, and a stray keystroke over a file being read is not an edit.
async function unlock(page) {
    if ((await page.locator("#source-lock").getAttribute("aria-pressed")) !== "true") {
        await page.locator("#source-lock").click();
    }
}

// The whole file the pane is holding, as an expression the browser evaluates.
//
// Read off the editor and not off the page. The pane is CodeMirror, which renders the lines
// that are on screen and no others, so its DOM is a window onto a file and never the file.
// `#source-view.editor` is the handle the pane puts there for exactly this. Written as text
// because a `waitForFunction` closure is serialised on its own and cannot call a helper that
// lives out here.
const SOURCE = 'document.getElementById("source-view").editor.state.doc.toString()';

function sourceText(page) {
    return page.evaluate(SOURCE);
}

// Whether the file in the pane is one somebody can type into.
function sourceIsLocked(page) {
    return page.evaluate(
        () => document.getElementById("source-view").editor.state.readOnly);
}

// Put the caret in the pane, on the editor's own content, which is what takes one.
async function clickIntoSource(page) {
    await page.locator(".cm-content").click();
}

// Type `line` in above the file's closing brace, which is where a declaration goes. The
// caret starts at the end of the file, which is past that brace, so it walks back one line
// first. Typing at the end would put the declaration outside the object it belongs to.
async function typeIntoRootBlock(page, line) {
    await unlock(page);
    await clickIntoSource(page);
    await page.keyboard.press("Control+End");
    await page.keyboard.press("ArrowUp");
    await page.keyboard.press("Home");
    await page.keyboard.type(line);
}

// The tree names each file by its leaf under a heading for the directory it is in, and that
// directory is the entity's own folder (`db/relational/store`), so this is what opens one: the
// leaf, under the whole folder rather than under its first segment.
function fileRow(page, name) {
    const parts = name.split("/");
    const leaf = parts[parts.length - 1];
    if (parts.length === 1) {
        return page.locator(`.tree > li > .tree__file`, { hasText: leaf });
    }
    // Directories nest in the tree, and a chain with no fork in it is drawn folded onto one
    // row, so a row is found by the whole path it stands for rather than by the words in it.
    const folder = parts.slice(0, -1).join("/");
    return page.locator(`li.tree__folder[data-folder="${folder}"] > .tree__leaves`)
               .locator("> li > .tree__file", { hasText: leaf });
}

// Open a file in the pane and wait for it to hold `wanted`, saying what it held instead when
// it never does. A timeout that only said "timed out" would throw away the file that is the
// whole answer.
async function openAndWaitFor(page, file, wanted) {
    await fileRow(page, file).click();
    try {
        await page.waitForFunction(`${SOURCE}.includes(${JSON.stringify(wanted)})`,
                                   null, { timeout: 15000 });
    } catch (error) {
        throw new Error(`${file} never held "${wanted}". It held:\n`
                        + await sourceText(page));
    }
}

// A connect point is drawn from one of the owner's handles to the consumer's disc, and the
// direction is its meaning, so this is the one interaction the page has that a keyboard cannot
// reach. `handle` picks a side: right, left, top, bottom, in that order.
async function dragLink(page, fromEntity, toEntity, handle = 0) {
    const rim = await page.locator(`[data-rim="${fromEntity}"]`).nth(handle).boundingBox();
    const target = await discCentre(page, toEntity);
    const start = { x: rim.x + (rim.width / 2), y: rim.y + (rim.height / 2) };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    // Through a point in between. The page treats a pointer that never travelled as a click.
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

// The same gesture, let go over empty canvas. What the page answers with an offer to make the
// entity that was being reached for.
async function dragLinkToNowhere(page, fromEntity, at, handle = 0) {
    const rim = await page.locator(`[data-rim="${fromEntity}"]`).nth(handle).boundingBox();
    const canvas = await page.locator("#canvas").boundingBox();
    const start = { x: rim.x + (rim.width / 2), y: rim.y + (rim.height / 2) };
    const target = { x: canvas.x + at.x, y: canvas.y + at.y };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

async function editorOverAProject() {
    console.log("\nThe editor over a project (synqt design)");
    const project = await copyProject();
    const { proc, url } = await startEditor(project);
    const browser = await chromium.launch({ headless });
    // Wide enough that an entity dropped below the three already there is still somewhere a
    // pointer can reach. The canvas is the middle column, with the palette and the panel
    // taking a fixed width off either side of it.
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const problems = [];
    page.on("pageerror", (error) => problems.push(String(error)));
    page.on("console", (message) => {
        if (message.type() === "error") {
            problems.push(message.text());
        }
    });
    try {
        await page.goto(url);
        await page.waitForFunction(
            () => document.getElementById("project").textContent === "gavel");
        check(await page.locator("[data-entity]").count() === 3,
              "the project on disk arrives as three entities on the canvas");

        // Add a service, and connect it to the edge. The service owns the point, the edge
        // consumes it. Dropped the other way round the page would draw a different project.
        await dropEntity(page, "Service", { x: 430, y: 400 });
        await page.waitForSelector('[data-entity="service"]');
        await dragLink(page, "service", "edge");
        // gavel exports two connect points, one on each of its two service entities. This
        // adds the third.
        await page.waitForFunction(
            () => document.querySelectorAll("[data-link]").length === 3);

        // A connect point is not named. Its owner names it, so the link that arrives is
        // `service`, carrying the `Service` type, and the panel offers nothing to
        // call it.
        const inspector = page.locator("#inspector");
        check(await page.locator('[data-link="service"]').count() === 1,
              "a new connect point is named after the entity that exports it");
        check(await inspector.locator("input[type=text]").count() === 0,
              "and the panel offers no name for it, because there is none to give");

        // A member is declared on the entity, in the entity's own file, and it crosses a
        // connect point because somebody ticked it there. Those are two gestures on two
        // different things, and the panel only ever offers the one that belongs to what is
        // selected. The line into a point offers neither.
        await page.locator('[data-entity="service"]').click();
        // Annotated, which is the form the guide asks for and the form that carries a type
        // onto the contract. An unannotated parameter is `var`, because the file
        // does not say what it is.
        await typeIntoRootBlock(page, "function logWinner(winner: string) {}");

        // The contract icon is the point. Clicking it is what opens what crosses, and the
        // list it opens is ticked out of what the owner declares and nothing else.
        //
        // Said in the contract's own vocabulary, not the QML the owner writes it in: this list
        // is the `export:` block, where a function is a `slot`. It used to say `function` for a
        // member read out of the owner's file and `slot` for one that only exists on the
        // point, so a single list carried two words for the same thing.
        await page.locator('[data-contract="service"]').click();
        const ticks = inspector.locator(".ticks");
        check(await ticks.getByText("slot logWinner(string winner)", { exact: true })
                         .count() === 1,
              "the contract offers what the owner declares, to tick");
        await ticks.locator("label.check", { hasText: "logWinner" })
                   .locator("input[type=checkbox]").check();

        // Nothing is written until a change set has been read, and Apply names the one that
        // was shown. It is refused until Review has been through the server.
        check(await page.locator("#apply").isDisabled(),
              "Apply is refused until the change set has been reviewed");
        await page.locator("#review").click();
        await page.waitForSelector("#sheet:not([hidden])");
        const diff = await page.locator("#sheet-diff").textContent();
        check(diff.includes("synqt.yaml") && diff.includes("Service.qml"),
              "the change set shows the configuration and the Source it would write");
        check(!fs.existsSync(path.join(project, "service/service/Service.qml")),
              "reviewing wrote nothing");

        await page.waitForSelector("#apply:not([disabled])");
        await page.locator("#apply").click();
        await waitForHint(page, "Applied");

        const config = await fsp.readFile(path.join(project, "synqt.yaml"), "utf8");
        check(/name:\s*service\b/.test(config), "synqt.yaml gained the service entity");
        check(/owner:\s*service\b/.test(config),
              "and the connect point it exports, owned by the entity it was dragged from");
        const points = (config.split(/^connect_points:$/m)[1] || "");
        check(!/^\s+name:/m.test(points),
              `written with no name of its own, because the owner is the name:\n${points}`);
        // What crosses a connect point is written on the point, in synqt.yaml, and nowhere
        // else. There is no contract file of its own to go looking in.
        const drawn = /slot\s+logWinner\s*\(\s*string\s+winner\s*\)/.test(config);
        check(drawn, drawn ? "the point's export block holds the slot that was drawn"
                           : `synqt.yaml holds something else:\n${config.trim()}`);
        check(fs.existsSync(path.join(project, "service/service/Service.qml")),
              "the owner got the Source file the point needs");
        // Its own file too, which is a different question. A Source is one surface an entity
        // exposes, and the entity is the thing that is there once. A plain service must not
        // arrive with an empty directory beside it.
        check(fs.existsSync(path.join(project, "service/service/Service.qml")),
              "and the file the entity itself is");
        check(problems.length === 0,
              `nothing on the page failed (${problems.join(" | ") || "no errors"})`);
    } finally {
        await browser.close();
        proc.kill("SIGINT");
        await fsp.rm(path.dirname(project), { recursive: true, force: true });
    }
}

// The copy the site publishes

const CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    // The mark in the bar and the tab icon. Served as what they are, the way a real static
    // host does. An <img> handed application/octet-stream is a broken image, and a broken
    // image in the harness is a page nobody is looking at.
    ".svg": "image/svg+xml",
    ".ico": "image/vnd.microsoft.icon",
};

// The static host the documentation site is, near enough. It serves the files the hook
// copies and knows nothing about /api/, so the editor's first request is answered with a
// 404 and the page has to turn itself into a drawing board on the strength of that.
function serveAssets() {
    return new Promise((resolve) => {
        const server = createServer((request, response) => {
            const name = (request.url || "/").split("?")[0].replace(/^\//, "") || "index.html";
            const file = path.join(assets, path.normalize(name));
            if (!file.startsWith(assets) || !fs.existsSync(file)) {
                response.writeHead(404, { "Content-Type": "text/plain" });
                response.end("not found");
                return;
            }
            response.writeHead(200, {
                "Content-Type": CONTENT_TYPES[path.extname(file)] || "application/octet-stream",
                "Content-Security-Policy": "default-src 'self'",
            });
            response.end(fs.readFileSync(file));
        });
        server.listen(0, "127.0.0.1", () => resolve(server));
    });
}

// Drag from a seat on a front's flat side onto an entity, which is how a scope is handed to
// the one that serves it. The seats are always visible, unlike the rim handles.
async function dragSeat(page, front, scope, toEntity) {
    const seat = await page.locator(`[data-seat="${front}"][data-scope="${scope}"]`)
                           .boundingBox();
    const target = await discCentre(page, toEntity);
    const start = { x: seat.x + (seat.width / 2), y: seat.y + (seat.height / 2) };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

// The other direction. A line pulled off an entity and let go on the word naming a scope on
// the front's back. Aimed at the label rather than the dot beside it, because that is the
// part of a seat somebody can see and hit.
async function dropOnSeat(page, fromEntity, front, scope) {
    const rim = await page.locator(`[data-rim="${fromEntity}"]`).nth(0).boundingBox();
    const label = await page.locator(`[data-entity="${front}"] .node__seat-name`)
                            .filter({ hasText: scope }).first().boundingBox();
    const start = { x: rim.x + (rim.width / 2), y: rim.y + (rim.height / 2) };
    const target = { x: label.x + (label.width / 2), y: label.y + (label.height / 2) };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

// A seat dragged onto empty canvas, which is how a scope is taken off the entity serving it.
async function unwireSeat(page, front, scope, at) {
    const seat = await page.locator(`[data-seat="${front}"][data-scope="${scope}"]`)
                           .boundingBox();
    const canvas = await page.locator("#canvas").boundingBox();
    const start = { x: seat.x + (seat.width / 2), y: seat.y + (seat.height / 2) };
    const target = { x: canvas.x + at.x, y: canvas.y + at.y };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

// Put the pointer on a link's own curve, `along` of the way from its owner. Aimed off the
// path itself rather than at a bounding box, because a link is a bowed curve and the middle
// of the box it sits in is usually canvas.
async function hoverAlong(page, link, along) {
    const at = await page.evaluate(([name, fraction]) => {
        const path = document.querySelector(`[data-link="${name}"] .link__hit`);
        const on = path.getPointAtLength(path.getTotalLength() * fraction);
        const point = document.getElementById("canvas").createSVGPoint();
        point.x = on.x;
        point.y = on.y;
        const screen = point.matrixTransform(path.getScreenCTM());
        return { x: screen.x, y: screen.y };
    }, [link, along]);
    await page.mouse.move(at.x - 4, at.y - 4);
    await page.mouse.move(at.x, at.y);
    return at;
}

// The break on a line into a front, dragged onto the word naming a scope: the fix for what the
// cross says, done from the cross itself.
async function dragBreak(page, link, front, scope) {
    const mark = await page.locator(`[data-break="${link}"]`).boundingBox();
    const label = await page.locator(`[data-entity="${front}"] .node__seat-name`)
                            .filter({ hasText: scope }).first().boundingBox();
    const start = { x: mark.x + (mark.width / 2), y: mark.y + (mark.height / 2) };
    const target = { x: label.x + (label.width / 2), y: label.y + (label.height / 2) };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

// A browser owns nothing, and a front is a web edge that hands its callers on. Both are drawn
// rather than configured, so both are checked the way somebody would do them.
async function theFrontThatSplitsCallers() {
    console.log("\nDrawing a front, and a link a browser cannot own");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage();
    const refused = [];
    page.on("pageerror", (error) => refused.push(String(error)));
    page.on("console", (message) => {
        const from = (message.location() || {}).url || "";
        if (message.type() === "error" && !from.endsWith("/api/project")) {
            refused.push(`${message.text()} (${from})`);
        }
    });
    try {
        await page.goto(`${origin}/index.html`);
        await page.waitForFunction(
            () => document.getElementById("apply").textContent === "Download");
        await dropEntity(page, "Client", { x: 80, y: 110 });
        await page.waitForSelector('[data-entity="client"]');
        await dropEntity(page, "Web edge", { x: 330, y: 110 });
        await page.waitForSelector('[data-entity="web"]');
        await dropEntity(page, "Service", { x: 590, y: 260 });
        await page.waitForSelector('[data-entity="service"]');

        // Drawn from the browser, which cannot own a connect point. The page turns it round
        // rather than drawing something that could never be built.
        await dragLink(page, "client", "web");
        await page.waitForSelector("[data-link]");
        // A point is named after its owner, so the name is the assertion: drawn from the
        // browser, it comes out owned by the edge.
        check(await page.locator('[data-link="web"]').count() === 1,
              "a link drawn from the browser is turned round and owned by the edge");
        check(await page.locator('[data-link="client"]').count() === 0,
              "and the one a browser could never host is not drawn");
        // A new point opens the picker asking what crosses it, and it sits over the
        // canvas. Clicking empty canvas puts it away, which is what anybody would do.
        const canvasBox = await page.locator("#canvas").boundingBox();
        await page.mouse.click(canvasBox.x + 660, canvasBox.y + 30);

        // Turned into a front on the edge's own panel, which is where what an edge does is
        // asked about, and wired on the canvas.
        await page.locator('[data-entity="web"]').click();
        await page.waitForSelector('[data-entity="web"].is-selected');
        await page.locator(".check", { hasText: "Hands callers to the entities behind it" })
                  .locator("input").check();
        await page.waitForSelector('[data-entity="web"] .node__wedge');
        check(await page.locator('[data-seat="web"]').count() === 4,
              "a front is drawn as a wedge with a seat for every scope");
        check(await page.locator('[data-entity="web"] .node__seat.is-taken').count() === 0,
              "and every seat starts empty, which is the question the drawing asks");

        await dragSeat(page, "web", "admin", "service");
        await page.waitForSelector('[data-entity="web"] .node__seat.is-taken');
        check(await page.locator('[data-entity="web"] .node__seat.is-taken').count() === 1,
              "dragging from the admin seat fills it in");
        await page.waitForSelector('[data-link="service"]');
        check(true, "with the connect point the front consumes drawn at the same time");
        // The seat keeps its own name whether or not anything is wired to it. Giving
        // the name up to the line that lands on it puts it a dozen pixels from the
        // neighbouring seat's name and leaves the two unreadable together.
        check(await page.locator('[data-entity="web"] .node__seat-name')
                        .filter({ hasText: "admin" }).count() === 1,
              "and the seat still says which scope it is");

        // And it is in the file, which is the only place any of it means anything.
        await fileRow(page, "synqt.yaml").click();
        await page.waitForFunction(`${SOURCE}.includes("behind:")`);
        const written = await sourceText(page);
        check(/behind:\s*\n\s*admin: service/.test(written),
              "and written as 'behind: admin: service' in synqt.yaml");

        // A link into a front that no scope hands anyone to. The front stopped answering its
        // own point the moment the switch went on, so a line arriving at it that nothing is
        // routed to carries nobody, and the drawing says so rather than leaving it looking
        // like an ordinary link. It is severed three quarters of the way along, under a cross
        // and the word.
        //
        // The cross is also the handle. Dragging it onto a scope is the fix for exactly what
        // it says, which is the whole reason it is a thing to point at rather than a colour.
        await unwireSeat(page, "web", "admin", { x: 120, y: 420 });
        await page.waitForSelector('[data-break="service"]');
        check(await page.locator('[data-link="service"]').count() === 1,
              "a link nothing routes to stays on the canvas");
        check(await page.locator('[data-break="service"] .link__break-word')
                        .first().textContent() === "broken",
              "and says it is broken, on the line, at the end it fails to reach");
        // Said by the whole line, not only by the cross on it. The cross is one mark near the
        // far end of a curve crossing the canvas, and the line is what a pointer reaching for
        // it lands on. Answering the line with the ordinary card had the drawing describe a
        // link that carries nobody as though it worked.
        await hoverAlong(page, "service", 0.3);
        await page.waitForSelector("#tip:not([hidden])");
        check((await page.locator("#tip .tip__kind").first().textContent()) === "broken",
              "and pointing anywhere along it says the same thing the cross does");
        // The travelling dash stops there too. It draws travel, and past the break nothing
        // travels. Running it the whole way was the animation contradicting the line under it.
        const carried = await page.evaluate(() => {
            const of = (name) => Math.round(document
                .querySelector(`[data-link="service"] ${name}`).getTotalLength());
            return {flow: of(".link__flow"), whole: of(".link__hit")};
        });
        check(carried.flow < carried.whole,
              `and the dash that runs stops at the break (${carried.flow} of ${carried.whole})`);
        // Pressing the cross is pressing the line, one gesture and one result. It must not select
        // the connect point and start drawing a line out of the cross, as though the cross
        // were an entity somebody had reached for.
        await page.locator('[data-break="service"]').click();
        await page.waitForSelector('[data-link="service"].is-selected');
        check(await page.locator("#ghost *").count() === 0,
              "and pressing it selects the line rather than starting one");
        await dragBreak(page, "service", "web", "moderator");
        await page.waitForFunction(
            () => document.querySelectorAll("[data-break]").length === 0);
        check(await page.locator('[data-entity="web"] .node__seat.is-taken').count() === 1,
              "and dragging the break onto a scope is what mends it");
        await unwireSeat(page, "web", "moderator", { x: 120, y: 420 });
        await page.waitForSelector('[data-break="service"]');
        await dragSeat(page, "web", "admin", "service");
        await page.waitForFunction(
            () => document.querySelectorAll("[data-break]").length === 0);

        // The other way round, which is the way anybody reaches for it, a line pulled off the
        // entity and let go on the word naming the scope. A drop test that is a circle around
        // the middle of a node lands this on nothing, because the seats and their
        // names are along its back.
        await dropOnSeat(page, "service", "web", "user");
        await page.waitForFunction(() => document.getElementById("hint").textContent
            .includes("Callers holding 'user'"));
        check(await page.locator('[data-entity="web"] .node__seat.is-taken').count() === 2,
              "a line let go on a scope hands that scope to the entity it came from");

        // And taking the link away takes the routing with it. The two are one declaration,
        // and a seat left filled with no line to it is a drawing of something that is not
        // there. The seat's name is still there, since the seat keeps it either way.
        await page.locator('[data-contract="service"]').click();
        await page.locator("button", { hasText: "Delete connect point" }).click();
        await page.waitForFunction(() => document.querySelectorAll(
            '[data-entity="web"] .node__seat.is-taken').length === 0);
        check(true, "and deleting that link frees every seat it was wired to");
        await fileRow(page, "synqt.yaml").click();
        // The routing is gone and the switch is not. The edge is still a front, now with
        // nothing behind it, and that is what `behind: {}` says. Written as no key at all, the
        // canvas drew a wedge with four seats and handed over a file describing a plain edge,
        // and `synqt check` had nothing to warn about because there was no front in the file
        // to warn about.
        await page.waitForFunction(`${SOURCE}.includes("behind: {}")`);
        check(!/behind:\s*\n\s*\w+:/.test(
                  await sourceText(page)),
              "with the routing gone from synqt.yaml and the front still declared");

        check(refused.length === 0,
              `the policy refuses nothing on the page (${refused.join(" | ") || "no errors"})`);
    } finally {
        await browser.close();
        server.close();
    }
}

async function theCopyOnTheSite() {
    console.log("\nThe copy the site publishes (no server behind it)");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage();
    const offOrigin = [];
    const refused = [];
    page.on("request", (request) => {
        if (!request.url().startsWith(origin)) {
            offOrigin.push(request.url());
        }
    });
    page.on("pageerror", (error) => refused.push(String(error)));
    page.on("console", (message) => {
        // The 404 on api/project is the whole point of this case: it is how the page finds
        // out there is nobody behind it. Chromium reports a failed fetch as a console error
        // whose text names no URL, so it is matched on where it came from, and only that
        // one is dropped: everything else this case exists to catch is kept.
        const from = (message.location() || {}).url || "";
        if (message.type() === "error" && !from.endsWith("/api/project")) {
            refused.push(`${message.text()} (${from})`);
        }
    });
    try {
        await page.goto(`${origin}/index.html`);
        await page.waitForFunction(
            () => document.getElementById("apply").textContent === "Download");
        check(await page.locator("#review").isHidden(),
              "with no project to write to, Review is not offered");
        check(await page.locator("#infer").isHidden(),
              "and neither is reading contracts back out of a project that is not there");
        check(await page.locator("#apply").isEnabled(), "Apply became the download");

        // It is still an editor: the palette works and the rules paint.
        await dropEntity(page, "Client", { x: 300, y: 200 });
        await page.waitForSelector('[data-entity="client"]');
        // Under Review in the rail, which is the only place a finding is said: the bar
        // carries the project's name and no verdict, because a count there was the same news
        // with nowhere to click through to.
        const findings = await page.locator("#findings").textContent();
        check(/web edge/.test(findings),
              `a client with no web edge is a problem the page paints for itself: ${findings}`);
        check(await page.locator("#verdict").count() === 0,
              "and the bar carries no verdict of its own");

        check(offOrigin.length === 0,
              `nothing is fetched from another origin (${offOrigin.join(", ") || "none"})`);
        check(refused.length === 0,
              `the policy refuses nothing on the page (${refused.join(" | ") || "no errors"})`);
    } finally {
        await browser.close();
        server.close();
    }
}

// The parts of the editor that are only a browser. A project opened from the fragment, the
// two panes that read it, and the menu a right click opens. None of them can be reached from
// Python, and all of them are how somebody arriving from the site meets the editor.
async function theProjectALinkHandsYou() {
    console.log("\nA project opened from a link, and the panes that read it");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const refused = [];
    page.on("pageerror", (error) => refused.push(String(error)));
    page.on("console", (message) => {
        const from = (message.location() || {}).url || "";
        if (message.type() === "error" && !from.endsWith("/api/project")) {
            refused.push(`${message.text()} (${from})`);
        }
    });
    try {
        await page.goto(`${origin}/index.html#example=feed`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 4);
        check(await page.locator("#project").textContent() === "demo",
              "the fragment named a project and the page opened it");
        check(await page.locator("#links [data-link]").count() === 3,
              "with the connect points it declares");
        // One icon per point, whatever its consumer list holds: the icon is the point, and
        // every line into it leaves from underneath that one mark.
        check(await page.locator("#links [data-contract]").count() === 3,
              "each drawn with a single contract icon, not one per consumer");
        check(await page.locator(".palette__glyph svg").count() === 8,
              "every palette row carries the glyph the canvas draws that entity with");
        // In the page's own tooltip, not the browser's `title`: it opens at once and can
        // hold the glyph and the paragraph, where a native one arrives a second late with
        // one line of unstyled text.
        await page.locator(".palette__item").first().hover();
        await page.waitForSelector("#tip:not([hidden])");
        check((await page.locator("#tip").textContent()).includes("WebAssembly"),
              "and says what that kind of entity is for");

        // Which side of the wire each entity is on, drawn rather than left to be worked out
        // from which column it landed in.
        check(await page.locator("#zones .zone").count() === 3,
              "the browser, the entity facing the internet and the mesh are three boxes");
        check((await page.locator("#zones .zone--internet .zone__title").textContent())
              === "Faces the internet",
              "and the one that faces the internet says so");

        // Every link is a curve, so that two entities talking both ways, or one owning
        // several points another consumes, are lines somebody can tell apart.
        check(await page.locator("#links path.link__line").count() === 3,
              "the links are curves, not lines laid over each other");

        // What a line carries, as a block rather than as a stack of centred strings: one
        // left edge for the marks, one for the names, and a ground under both so a row
        // landing on a zone edge is still a row.
        const block = page.locator("#links .link__members")
            .filter({ has: page.locator("[data-member='loaded']") });
        const columns = await block.locator(".link__member").evaluateAll(
            (rows) => rows.map((row) => Math.round(row.getAttribute("x") * 100)));
        check(columns.length === 4 && new Set(columns).size === 1,
              `every member of a link starts in one column (${columns.join(" ")})`);
        const marks = await block.locator(".link__mark").evaluateAll(
            (rows) => rows.map((row) => row.dataset.kind));
        check(marks.join(" ") === "prop model slot signal",
              `each with the mark for what it is, and the word gone (${marks.join(" ")})`);
        check(!(await block.locator(".link__member").first().textContent()).includes("prop"),
              "so the row is the member and nothing else");
        check(await block.locator("rect.link__members-box").count() === 1,
              "over one ground the whole block sits on");
        // The word is off the canvas, so pointing at the shape is what has to say it.
        await block.locator("[data-member='denied'].link__mark").hover();
        await page.waitForSelector("#tip:not([hidden])");
        const said = await page.locator("#tip").textContent();
        check(said.includes("denied") && said.includes("signal"),
              `and hovering a mark says which of the four it is (${said.slice(0, 60)})`);

        // The pane is open with the page: the files are what is being designed, not a second
        // opinion about it that has to be asked for.
        await page.waitForSelector(".tree__file");
        check(!(await page.locator("#dock").evaluate(
                  (dock) => dock.classList.contains("is-collapsed"))),
              "the files pane is open without being asked for");
        // The whole path each row stands for, which is what the row carries: directories nest,
        // and a chain with no fork in it is drawn folded onto one row.
        const folders = await page.locator(".tree__folder").evaluateAll(
            (rows) => rows.map((row) => row.dataset.folder));
        // Each entity's own folder, whole: the folder its type puts it in and then its name.
        check(["client/app", "web/edge", "db/relational/store", "api/feeds"]
                  .every((name) => folders.includes(name)),
              `the tree is entity directories, not one flat list (${folders.join(" ")})`);
        const named = await page.locator(".tree__file").allTextContents();
        // Every entity present, with its own file. A plain service used to contribute nothing
        // at all until somebody drew a connect point off it.
        const wanted = ["synqt.yaml", "Main.qml", "Edge.qml", "Store.qml", "Feeds.qml"];
        const missing = wanted.filter((name) => !named.includes(name));
        check(missing.length === 0,
              missing.length ? `the files pane is missing ${missing.join(", ")}; it names `
                               + named.join(", ")
                             : "every entity has its own file, and every file its directory");

        await fileRow(page, "web/edge/Edge.qml").click();
        const source = await sourceText(page);
        // Rooted at its own name: the edge exports `Edge`, and `Edge {}` is the Source
        // that answers it. One entity, one file, one name.
        check(source.includes("Edge {"),
              "and reading one shows the Source the owner would host");
        check(!source.includes("SPDX-License-Identifier"),
              "without the licence notice, which is on every file and read by nobody");
        check(await page.locator(".cm-content .tok--keyword").count() > 0,
              "coloured as the QML it is");

        // Opening a file selects what it is on the canvas, and the reverse, so the two views
        // never disagree about what is in hand. The entity: a file sits in an entity's folder
        // and is that entity's own code, whether or not a connect point is exported out of it.
        await page.waitForSelector("[data-entity='edge'].is-selected");
        check(true, "opening a file selects the entity it belongs to, out on the canvas");
        await page.locator("#nodes [data-entity='feeds']").click();
        await page.waitForFunction(
            () => document.getElementById("source-name").textContent === "api/feeds/Feeds.qml");
        check(true, "and selecting an entity opens the file it is");
        // That click was a redraw, with the pointer still on the entity it drew. The handles
        // a link is pulled from are put on whichever entity the pointer is nearest, and the
        // drawing is rebuilt from the document on every change: they used to go with it, so a
        // press where a handle had been a moment ago landed on the canvas behind it and
        // panned the view instead of starting a link.
        check(await page.locator("[data-entity='feeds'].is-near").count() === 1,
              "and the handles a link is drawn from survive the redraw that click causes");

        // Read-only until unlocked: the pane holds the entity's own code.
        await fileRow(page, "web/edge/Edge.qml").click();
        check(await sourceIsLocked(page), "a file opens read-only");
        // And the unlock belongs to the file, not to the pane. Opening another shows it
        // locked, and coming back finds the one you unlocked still open: as a flag it had to
        // be cleared by hand everywhere the pane could change underneath it, and every place
        // that forgot re-locked the file being typed into.
        await unlock(page);
        await fileRow(page, "synqt.yaml").click();
        const relocked = await sourceIsLocked(page);
        await fileRow(page, "web/edge/Edge.qml").click();
        check(relocked && !(await sourceIsLocked(page)),
              "unlocking one file locks none of the others, and lasts on the one it was for");
        // Typing a declaration into a Source is the same gesture as adding a member in the
        // panel, which is the whole reason the pane is a textarea and not a preview. It
        // declares; it does not export. A contract is the list of what an owner has agreed to
        // say to somebody else, and writing a property on an entity is not that agreement --
        // it is also how a half-typed name used to walk onto the wire one letter at a time.
        await typeIntoRootBlock(page, "    property string headline\n");
        await openAndWaitFor(page, "web/edge/Edge.qml", "property string headline");
        const exported = await sourceText(page);
        await fileRow(page, "synqt.yaml").click();
        await page.waitForFunction(
            () => document.getElementById("source-name").textContent === "synqt.yaml");
        check(!(await sourceText(page)).includes("prop string headline")
              && exported.includes("property string headline"),
              "a property typed into a Source is declared, and does not cross until it is "
              + "ticked");
        // And ticking it is what puts it there. The contract icon opens the point; the list
        // under it is everything the owner declares, with what crosses ticked.
        await page.locator("[data-contract='edge']").click();
        await page.locator(".tick label", { hasText: "prop string headline" })
                  .locator("input").check();
        await openAndWaitFor(page, "synqt.yaml", "prop string headline");
        check(true, "and ticking it on the connect point is what makes it cross");

        // Putting the caret on a line points the canvas at what that line is about, which is
        // how somebody reading a file finds the thing they are reading in the drawing.
        await fileRow(page, "web/edge/Edge.qml").click();
        await unlock(page);
        await clickIntoSource(page);
        await page.keyboard.press("Control+End");
        await page.keyboard.press("ArrowUp");
        await page.keyboard.press("ArrowUp");
        await page.waitForSelector("[data-link='edge'].is-selected");
        check(true, "the line the caret is on selects what it declares, out on the canvas");

        // Undo, and undo of what was typed rather than of everything the pane has been shown.
        // The editor is given a file at a time, and the pane is rebuilt from the document on
        // every keystroke. A history that spanned those would let somebody open a file, press
        // undo, and watch the file before it arrive in the pane.
        await unlock(page);
        await clickIntoSource(page);
        await page.keyboard.press("Control+End");
        await page.keyboard.type("// undo me");
        const before = (await sourceText(page)).includes("// undo me");
        await page.keyboard.press("Control+z");
        check(before && !(await sourceText(page)).includes("// undo me"),
              "and what was typed can be undone, the way any editor undoes");

        // Reaching into another entity puts the member on the connect point it would cross.
        // This is what `synqt infer` does over a project, done here on one file while it is
        // being typed: `Server` is the client's alias for the edge, so the edge's point is
        // what gains it.
        await fileRow(page, "client/app/Main.qml").click();
        await typeIntoRootBlock(page, "    property int seen: Server.tally\n");
        await openAndWaitFor(page, "synqt.yaml", "prop var tally");
        check(true, "reaching into another entity adds the member it reached for, with the "
                    + "type nothing gave away");
        // One member rather than one per letter. A name is typed a letter at a time, so every prefix
        // of it arrives here as a reference of its own. If each became a member and stayed
        // one, `tally` would cost the contract `t`, `ta`, `tal` and `tall` on the way.
        const halves = (await sourceText(page))
            .split("\n")
            .filter((row) => /^\s+prop var tall?y?$/.test(row));
        check(halves.length === 1 && halves[0].trim() === "prop var tally",
              `and the name typed to get there left nothing behind (${halves.join(" | ")})`);

        // A contract starts empty, and what the consumer's own code already reaches for is
        // the exception: code that is written is somebody having said so, and asking them to
        // tick a box for a call they have already made is asking them to say it twice.
        // The edge's file calls `Store.allows(...)`, so drawing that link back gives it back.
        await page.locator("[data-contract='store']").click();
        await page.locator(".inspector__actions .button--danger",
                           { hasText: "Delete connect point" }).click();
        await page.waitForFunction(
            () => document.querySelectorAll("[data-contract='store']").length === 0);
        await dragLink(page, "store", "edge");
        await page.waitForSelector("[data-contract='store']");
        await openAndWaitFor(page, "synqt.yaml", "owner: store");
        check((await sourceText(page)).includes("slot allows("),
              "a link drawn to a consumer that already calls into it carries what it calls");

        // The pane is an editor and not a box with text in it, and the difference is a list
        // of things a reader can point at. The one this asserts is the one everything else
        // relies on: the file's lines are numbered, so a finding, a stack trace or a
        // colleague saying "line 40" all point at something.
        await fileRow(page, "client/app/Main.qml").click();
        const numbered = await page.evaluate(() => {
            const view = document.getElementById("source-view").editor;
            // The first element of the gutter is a spacer holding the widest number there
            // will be, drawn to reserve the width and never read, so what is numbered is
            // everything after it.
            const shown = [...view.dom.querySelectorAll(".cm-lineNumbers .cm-gutterElement")]
                .slice(1).map((one) => one.textContent);
            return {shown: shown.slice(0, 3), lines: view.state.doc.lines};
        });
        check(numbered.shown.join(",") === "1,2,3" && numbered.lines > 3,
              `the pane numbers the file's lines (${numbered.shown.join(",")} of `
              + `${numbered.lines})`);
        // And the styles the editor builds for itself are inside the shadow root it lives in,
        // which is what lets it live under a policy that refuses an inline style at all.
        check(await page.evaluate(
                  () => document.getElementById("source-view").shadowRoot
                                .adoptedStyleSheets.length > 0),
              "and builds its own styles as a sheet the policy has no opinion about");

        // Pressed twice, because closing and opening again are two different failures. A
        // chevron rebuilt as part of closing detaches the element the click
        // landed on. The click then carries on to the strip, finds no button above it,
        // takes itself for a click on the strip and opens the pane again in the same turn. That
        // works every other time, which is the shape of bug a single press never catches.
        await page.click("#dock-toggle");
        check(await page.locator("#dock").evaluate(
                  (dock) => dock.classList.contains("is-collapsed")),
              "hiding the pane leaves the bar it opens again from");
        check(await page.locator("#dock-bar").isVisible()
              && await page.locator("#dock-pane").isHidden(),
              "which still names the file that was open");
        await page.click("#dock-toggle");
        check(!(await page.locator("#dock").evaluate(
                   (dock) => dock.classList.contains("is-collapsed"))),
              "and pressing it again opens it, every time and not every other time");
        await page.waitForSelector(".tree__file");

        // A link dropped on empty canvas is somebody reaching for an entity that is not there
        // yet, which is an offer rather than a mistake. Pulled off the *left* handle, which
        // is the side a single handle on the right could never serve.
        const lines = await page.locator("#links > *").count();
        await dragLinkToNowhere(page, "store", { x: 90, y: 90 }, 1);
        await page.waitForSelector(".menu__item");
        await page.locator(".menu__item", { hasText: "Cache" }).click();
        await page.waitForSelector("[data-entity='cache']");
        check(await page.locator("#links > *").count() === lines + 1,
              "a link dropped on nothing offers to make the entity it was reaching for");
        // And it joined the consumer list of the point `store` already exports, because an
        // entity has one: a second line out of an owner is another consumer, not another
        // point.
        const both = await page.locator("[data-link='store']").count();
        check(both === 2,
              `as a second consumer of the one connect point that owner exports (${both})`);

        // Delete removes what is selected, and a double click renames it.
        await page.locator("#nodes [data-entity='cache']").click();
        await page.locator("#canvas").press("Delete");
        await page.waitForFunction(
            () => !document.querySelector("#nodes [data-entity='cache']"));
        check(true, "Delete removes what is selected");

        // In place, over the entity, and not in a dialog: the field opens where the name was,
        // holding it, and Enter is what commits. A prompt would cover the drawing the new
        // name is being chosen against, which is the only thing anybody is looking at.
        await page.locator("#nodes [data-entity='feeds']").dblclick();
        await page.waitForSelector(".rename");
        check(await page.locator(".rename").inputValue() === "feeds",
              "a double click opens the name where the name is");
        await page.locator(".rename").fill("upstream");
        await page.locator(".rename").press("Enter");
        await page.waitForSelector("[data-entity='upstream']");
        check(await page.locator("#nodes [data-entity='feeds']").count() === 0
              && await page.locator(".rename").count() === 0,
              "and typing a new one there renames it");

        // The tooltip is the page's own, so it can say what a native one cannot.
        await page.locator("#nodes [data-entity='store']").hover();
        await page.waitForSelector("#tip:not([hidden])");
        const tip = await page.locator("#tip").textContent();
        check(tip.includes("store") && tip.includes("consumer lists"),
              `hovering an entity says what can reach it (${tip.slice(0, 80)})`);

        // Deleting an entity takes every line that only existed because it was there: the
        // points it owned, and the points whose one consumer it was. A point left with an
        // empty consumer list is drawn as a stub from its owner to nothing, which is what a
        // point somebody deliberately disconnected looks like and is not what deleting the
        // thing at the other end means. `edge` owns `feed` and consumes `access`, so both go.
        await page.locator("#nodes [data-entity='edge']").click({ button: "right" });
        await page.waitForSelector(".menu__item");
        check(await page.locator(".menu__what").textContent() === "edge",
              "a right click opens a menu over what it was opened on");
        await page.locator(".menu__item", { hasText: "Delete" }).click();
        await page.waitForFunction(
            () => !document.querySelector("#nodes [data-entity='edge']"));
        check(await page.locator("#nodes [data-entity]").count() === 3,
              "and Delete there removes it");
        // Nothing is left pointing at it. `edge` owned one of the points still drawn and was
        // the only consumer of the rest, so all of them go: a point left with an empty
        // consumer list is a stub from its owner to nothing, which is what somebody
        // disconnecting a line asks for and not what deleting the far end means.
        const left = await page.locator("#links > *").count();
        check(left === 0,
              `taking with it every connect point that ran to or from it (${left} left)`);

        check(refused.length === 0,
              `the policy refuses nothing on the page (${refused.join(" | ") || "no errors"})`);
    } finally {
        await browser.close();
        server.close();
    }
}

// The two ways of saying the same thing that neither the canvas nor a Source covers: the panel
// declaring on an entity, and the configuration typed into directly. Both write into the one
// document the rest of the page reads, so both are checked by what the page says afterwards.
async function typingIntoTheProject() {
    console.log("\nDeclaring from the panel, and typing into the configuration");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const refused = [];
    page.on("pageerror", (error) => refused.push(String(error)));
    page.on("console", (message) => {
        const from = (message.location() || {}).url || "";
        if (message.type() === "error" && !from.endsWith("/api/project")) {
            refused.push(`${message.text()} (${from})`);
        }
    });
    try {
        await page.goto(`${origin}/index.html#example=feed`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 4);

        // Declaring on an entity, from the panel. This is the pool every connect point the
        // entity owns ticks its contract from, and it used to be reachable only by typing the
        // line into the file: the panel offered a free-text prompt on a *link* and nothing at
        // all on an entity.
        await page.locator("#nodes [data-entity='store']").click();
        const declares = page.locator(".members").filter(
            { hasText: "What this entity declares" });
        await declares.getByRole("button", { name: "property", exact: true }).click();
        await page.waitForSelector(".member__kind");
        const kinds = await declares.locator(".member__kind").allTextContents();
        check(kinds.includes("property"),
              `the panel declares on the entity itself (${kinds.join(" | ")})`);
        // A model, which has no QML declaration form and is written onto the point instead.
        // It is declared here all the same. It is one of the four things an entity declares,
        // and being the odd one out of the four is not a reason to keep it somewhere else.
        await declares.getByRole("button", { name: "model", exact: true }).click();
        await page.waitForFunction(
            () => document.querySelectorAll(".member__kind").length > 1);
        const withModel = await declares.locator(".member__kind").allTextContents();
        check(withModel.includes("model"),
              `a model is declared on the entity too (${withModel.join(" | ")})`);

        // Every part of a declaration that comes out of a fixed list is chosen from that
        // list. The type is a drop-down over the contract vocabulary, not a word to be typed
        // into the file afterwards. The name is the one part that has to be typed, because
        // nothing could offer it. Both edits go back into the line they were read from.
        // The property that was added, picked out by its kind. The entity's own file
        // already declares a function, so "the first one" is whatever the file happens to
        // open with rather than the row this is about.
        const row = declares.locator(".member").filter({ hasText: "property" }).first();
        await row.locator("input[type=text]").fill("shelfCount");
        await row.locator("select").selectOption("string");
        await openAndWaitFor(page, "db/relational/store/Store.qml",
                             "property string shelfCount");
        check(true, "and the declaration is in the entity's own file, name and type together");

        // Typing into synqt.yaml. The canvas follows what parses. What does not parse leaves
        // the canvas alone and says which line stopped it, because half-typed text is the
        // ordinary state of a file being edited and not an error to undo.
        await fileRow(page, "synqt.yaml").click();
        await unlock(page);
        check(await page.locator("#revert").isVisible(),
              "the way back is offered before the first keystroke, not after it");
        await clickIntoSource(page);
        await page.keyboard.press("Control+End");
        // A second point on an owner that already exports one: drawn, because it is what the
        // file says, and marked, because an entity has one connect point and the later entry
        // would quietly replace the first.
        const before = await page.locator("[data-link='store']").count();
        await page.keyboard.type("\n  - owner: store\n    consumers: [edge]\n");
        await page.waitForFunction(
            (was) => document.querySelectorAll("[data-link='store']").length > was, before);
        check(true, "a connect point typed into the configuration is drawn on the canvas");
        await page.waitForSelector("#findings .finding--error");
        const said = await page.locator("#findings .finding--error").first().textContent();
        check(said.includes("store") && said.includes("two connect points"),
              `and a second point on one owner is refused on the canvas (${said.trim()})`);

        await page.keyboard.press("Control+End");
        await page.keyboard.type("  - owner:\n");
        await waitForHint(page, "synqt.yaml, line");
        check(await page.locator("[data-entity]").count() === 4,
              "a line that does not read leaves the canvas on the last one that did");

        await page.locator("#revert").click();
        await waitForHint(page, "Back to the last version");
        check(await page.locator("[data-link='store']").count() > before,
              "and the way back returns the last version that read, keeping the work");

        check(refused.length === 0,
              `the policy refuses nothing on the page (${refused.join(" | ") || "no errors"})`);
    } finally {
        await browser.close();
        server.close();
    }
}

await editorOverAProject();
await theCopyOnTheSite();
await theProjectALinkHandsYou();
await theFrontThatSplitsCallers();
await typingIntoTheProject();

console.log("");
if (failures.length) {
    console.log(`designer: ${failures.length} check(s) failed`);
    process.exit(1);
}
console.log("designer: the editor writes what it draws, and the hosted copy stands alone");
