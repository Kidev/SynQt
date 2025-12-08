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

// Type `line` in just above the file's closing brace, which is where a declaration goes. The
// caret starts at the end of the file, which is past that brace, so it walks back one line
// first; typing at the end would put the declaration outside the object it belongs to.
async function typeIntoRootBlock(page, line) {
    await unlock(page);
    const typing = page.locator("#source-input");
    await typing.click();
    await typing.press("Control+End");
    await typing.press("ArrowUp");
    await typing.press("Home");
    await typing.type(line);
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
    const folder = parts.slice(0, -1).join("/");
    return page.locator("li.tree__folder")
               .filter({ hasText: new RegExp(`^${folder}/`) })
               .locator(".tree__file", { hasText: leaf });
}

// Open a file in the pane and wait for it to hold `wanted`, saying what it held instead when
// it never does. A timeout that only said "timed out" would throw away the file that is the
// whole answer.
async function openAndWaitFor(page, file, wanted) {
    await fileRow(page, file).click();
    try {
        await page.waitForFunction(
            (text) => document.getElementById("source-paint").textContent.includes(text),
            wanted, { timeout: 15000 });
    } catch (error) {
        const held = await page.locator("#source-paint").textContent();
        throw new Error(`${file} never held "${wanted}". It held:\n${held}`);
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
        await page.waitForFunction(
            () => document.querySelectorAll("[data-link]").length === 4);

        // A new point is named for the direction it runs, so this one arrives as
        // `serviceToWeb`. Renamed here to what it actually carries, which is the gesture the
        // name is a starting point for.
        const inspector = page.locator("#inspector");
        check(await inspector.locator("input[type=text]").first().inputValue()
              === "serviceToEdge",
              "a new connect point is named for the direction it runs");
        // Only the point is named. Its contract is that name capitalised and is not asked
        // for twice: `audit` exports `Audit`.
        await inspector.locator("input[type=text]").first().fill("audit");

        await inspector.getByText("Add member", { exact: true }).click();
        const member = inspector.locator(".member").first();
        await member.locator(".member__row select").first().selectOption("slot");
        // A member arrives as a prop, and a prop's type has a place to go on a slot: it
        // becomes the return type. This one answers with nothing, so say so.
        await member.locator(".member__row select").nth(1).selectOption("");
        await member.locator(".member__row input[type=text]").first().fill("logWinner");
        await member.getByText("Add parameter", { exact: true }).click();
        const parameter = member.locator(".member__part").first();
        await parameter.locator("select").selectOption("string");
        await parameter.locator("input[type=text]").fill("winner");

        // Nothing is written until a change set has been read, and Apply names the one that
        // was shown: it is refused until Review has been through the server.
        check(await page.locator("#apply").isDisabled(),
              "Apply is refused until the change set has been reviewed");
        await page.locator("#review").click();
        await page.waitForSelector("#sheet:not([hidden])");
        const diff = await page.locator("#sheet-diff").textContent();
        check(diff.includes("synqt.yaml") && diff.includes("Audit.qml"),
              "the change set shows the configuration and the Source it would write");
        check(!fs.existsSync(path.join(project, "service/service/Audit.qml")),
              "reviewing wrote nothing");

        await page.waitForSelector("#apply:not([disabled])");
        await page.locator("#apply").click();
        await waitForHint(page, "Applied");

        const config = await fsp.readFile(path.join(project, "synqt.yaml"), "utf8");
        check(/name:\s*service\b/.test(config), "synqt.yaml gained the service entity");
        check(/name:\s*audit\b/.test(config), "synqt.yaml gained the connect point");
        check(/owner:\s*service\b/.test(config),
              "the point is owned by the entity it was dragged from");
        // What crosses a connect point is written on the point, in synqt.yaml, and nowhere
        // else: there is no contract file of its own to go looking in.
        const drawn = /slot\s+logWinner\s*\(\s*string\s+winner\s*\)/.test(config);
        check(drawn, drawn ? "the point's export block holds the slot that was drawn"
                           : `synqt.yaml holds something else:\n${config.trim()}`);
        check(fs.existsSync(path.join(project, "service/service/Audit.qml")),
              "the owner got the Source file the point needs");
        // Its own file too, which is a different question: a Source is one surface an entity
        // exposes, and the entity is the thing that is there once. A plain service used to
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
        // A point is named for the direction it runs, so the name is the assertion: drawn
        // from the browser, it comes out owned by the edge.
        check(await page.locator('[data-link="webToClient"]').count() === 1,
              "a link drawn from the browser is turned round and owned by the edge");
        check(await page.locator('[data-link="clientToWeb"]').count() === 0,
              "and the one a browser could never host is not drawn");
        // A new point opens the picker asking what crosses it, and it sits over the
        // canvas; clicking empty canvas puts it away, which is what anybody would do.
        const canvasBox = await page.locator("#canvas").boundingBox();
        await page.mouse.click(canvasBox.x + 660, canvasBox.y + 30);

        // Turned into a front from the panel, wired on the canvas. Selected by opening the
        // Source that answers it, the same way the panes and the canvas agree elsewhere: an
        // SVG hit band has no box a click can be aimed at.
        await fileRow(page, "web/web/WebToClient.qml").click();
        await page.waitForSelector('[data-link="webToClient"].is-selected');
        await page.locator(".check", { hasText: "Hand callers to entities behind it" })
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
        await page.waitForSelector('[data-link="serviceToWeb"]');
        check(true, "with the connect point the front consumes drawn at the same time");

        // And it is in the file, which is the only place any of it means anything.
        await fileRow(page, "synqt.yaml").click();
        await page.waitForFunction(
            () => document.getElementById("source-paint").textContent.includes("behind:"));
        const written = await page.locator("#source-paint").textContent();
        check(/behind:\s*\n\s*admin: service/.test(written),
              "and written as 'behind: admin: service' in synqt.yaml");

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
        const verdict = await page.locator("#verdict").textContent();
        check(/problem/.test(verdict),
              "a client with no web edge is a problem the page paints for itself");

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
        check(await page.locator("#project").textContent() === "my-app",
              "the fragment named a project and the page opened it");
        check(await page.locator("#links > *").count() === 3,
              "with the connect points it declares");
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

        // The pane is open with the page: the files are what is being designed, not a second
        // opinion about it that has to be asked for.
        await page.waitForSelector(".tree__file");
        check(!(await page.locator("#dock").evaluate(
                  (dock) => dock.classList.contains("is-collapsed"))),
              "the files pane is open without being asked for");
        // The row's own words, not everything under it: the heading also holds the entity's
        // glyph and, nested inside it, the list of that folder's files.
        const folders = await page.locator(".tree__folder").evaluateAll(
            (rows) => rows.map((row) => Array.from(row.childNodes)
                .filter((node) => node.nodeType === Node.TEXT_NODE)
                .map((node) => node.textContent).join("").trim()));
        // Each entity's own folder, whole: the folder its type puts it in and then its name.
        check(["client/app/", "web/edge/", "db/relational/store/", "api/feeds/"]
                  .every((name) => folders.includes(name)),
              `the tree is entity directories, not one flat list (${folders.join(" ")})`);
        const named = await page.locator(".tree__file").allTextContents();
        // Every entity present, with its own file. A plain service used to contribute nothing
        // at all until somebody drew a connect point off it.
        const wanted = ["synqt.yaml", "Main.qml", "Edge.qml", "Feed.qml", "Store.qml",
                        "Access.qml", "Feeds.qml", "Upstream.qml"];
        const missing = wanted.filter((name) => !named.includes(name));
        check(missing.length === 0,
              missing.length ? `the files pane is missing ${missing.join(", ")}; it names `
                               + named.join(", ")
                             : "every entity has its own file, and every file its directory");

        await fileRow(page, "web/edge/Feed.qml").click();
        const source = await page.locator("#source-paint").textContent();
        // Rooted at the contract's own type, which is the point's name capitalised: the
        // point `feed` exports `Feed`, and `Feed {}` is the Source that answers it.
        check(source.includes("Feed {"),
              "and reading one shows the Source the owner would host");
        check(!source.includes("SPDX-License-Identifier"),
              "without the licence notice, which is on every file and read by nobody");
        check(await page.locator("#source-paint .tok--keyword").count() > 0,
              "coloured as the QML it is");

        // Opening a file selects what it is on the canvas, and the reverse, so the two views
        // never disagree about what is in hand.
        await page.waitForSelector("[data-link='feed'].is-selected");
        check(true, "opening a file selects what it is out on the canvas");
        await page.locator("#nodes [data-entity='feeds']").click();
        await page.waitForFunction(
            () => document.getElementById("source-name").textContent === "api/feeds/Feeds.qml");
        check(true, "and selecting an entity opens the file it is");

        // Read-only until unlocked: the pane holds the entity's own code.
        await fileRow(page, "web/edge/Feed.qml").click();
        check(await page.locator("#source-input").evaluate((box) => box.readOnly),
              "a file opens read-only");
        // Typing a declaration into a Source is the same gesture as adding a member in the
        // panel, which is the whole reason the pane is a textarea and not a preview.
        await typeIntoRootBlock(page, "    property string headline\n");
        // Into the point's own `export:` block in synqt.yaml, which is where a contract lives.
        await openAndWaitFor(page, "synqt.yaml", "prop string headline");
        check(true, "a property typed into an unlocked Source becomes a member of its "
                    + "contract");

        // Putting the caret on a line points the canvas at what that line is about, which is
        // how somebody reading a file finds the thing they are reading in the drawing.
        await fileRow(page, "web/edge/Feed.qml").click();
        await unlock(page);
        await page.locator("#source-input").click();
        await page.locator("#source-input").press("Control+End");
        await page.locator("#source-input").press("ArrowUp");
        await page.locator("#source-input").press("ArrowUp");
        await page.waitForSelector("[data-link='feed'].is-selected");
        check(true, "the line the caret is on selects what it declares, out on the canvas");

        // Undo is the browser's own, and it only stays the browser's if the pane never writes
        // the box's value back over what was just typed into it: a programmatic write clears
        // the undo stack. Pressed rather than called, because that is the gesture.
        await unlock(page);
        const typed = page.locator("#source-input");
        await typed.click();
        await typed.press("Control+End");
        await typed.type("// undo me");
        const undone = await page.evaluate(async () => {
            const box = document.getElementById("source-input");
            return {before: box.value.includes("// undo me")};
        });
        await typed.press("Control+z");
        check(undone.before
              && !(await typed.inputValue()).includes("// undo me"),
              "and what was typed can be undone, the way any text box undoes");

        // Reaching into another entity draws the connect point it would need. This is what
        // `synqt infer` does over a project, done here on one file while it is being typed:
        // `Server` is the client's alias for the edge, so the edge is what ends up owning it.
        await fileRow(page, "client/app/Main.qml").click();
        await typeIntoRootBlock(page, "    property int seen: Server.tally.total\n");
        await page.waitForSelector("[data-link='tally']");
        check(true, "reaching into another entity draws the connect point that would carry it");
        const paths = await page.locator(".tree__file").allTextContents();
        check(paths.includes("Tally.qml"),
              `with the Source the owner would host it from (${paths.join(", ")})`);
        await openAndWaitFor(page, "synqt.yaml", "prop var total");
        check(true, "and the member it reached for, with the type nothing gave away");

        // A file longer than the pane is the ordinary case, and the coloured copy is a
        // separate layer from the one holding the caret, so the two have to move together.
        await fileRow(page, "client/app/Main.qml").click();
        // Right to the bottom, which is where the two used to come apart: the textarea keeps
        // room for a horizontal scrollbar and the copy behind it has none, so a copy that
        // scrolled clamped a scrollbar's height short. It is moved rather than scrolled now,
        // and what is asserted is that the copy sits exactly where the text went.
        const scrolled = await page.evaluate(() => {
            const input = document.getElementById("source-input");
            const paint = document.getElementById("source-paint");
            input.scrollTop = input.scrollHeight;
            input.dispatchEvent(new Event("scroll"));
            const moved = new DOMMatrixReadOnly(getComputedStyle(paint).transform);
            return {moved: input.scrollTop, painted: -moved.m42};
        });
        check(scrolled.moved > 0 && scrolled.painted === scrolled.moved,
              `the coloured copy moves with the caret (${scrolled.painted} of `
              + `${scrolled.moved})`);

        // Pressed twice, because closing and opening again are two different failures. The
        // chevron used to be rebuilt as part of closing, which detached the element the click
        // had landed on; the click then carried on to the strip, found no button above it,
        // took itself for a click on the strip and opened the pane again in the same turn. It
        // worked every other time, which is the shape of bug a single press never catches.
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
        await dragLinkToNowhere(page, "store", { x: 90, y: 90 }, 1);
        await page.waitForSelector(".menu__item");
        await page.locator(".menu__item", { hasText: "Cache" }).click();
        await page.waitForSelector("[data-entity='cache']");
        check(await page.locator("#links > *").count() === 5,
              "a link dropped on nothing offers to make the entity it was reaching for");

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
        await page.getByText("Add a property", { exact: true }).click();
        await page.waitForSelector(".declares__line");
        const declared = await page.locator(".declares__line").allTextContents();
        check(declared.some((line) => line.startsWith("property int ")),
              `the panel declares on the entity itself (${declared.join(" | ")})`);
        await openAndWaitFor(page, "db/relational/store/Store.qml", "property int");
        check(true, "and the declaration is in the entity's own file, where it lives");

        // Typing into synqt.yaml. The canvas follows what parses; what does not parse leaves
        // the canvas alone and says which line stopped it, because half-typed text is the
        // ordinary state of a file being edited and not an error to undo.
        await fileRow(page, "synqt.yaml").click();
        await unlock(page);
        check(await page.locator("#revert").isVisible(),
              "the way back is offered before the first keystroke, not after it");
        const box = page.locator("#source-input");
        await box.click();
        await box.press("Control+End");
        await box.type("\n  - name: audits\n    owner: store\n    consumers: [edge]\n");
        await page.waitForSelector("[data-link='audits']");
        check(true, "a connect point typed into the configuration is drawn on the canvas");

        await box.press("Control+End");
        await box.type("  - name:\n");
        await waitForHint(page, "synqt.yaml, line");
        check(await page.locator("[data-entity]").count() === 4,
              "a line that does not read leaves the canvas on the last one that did");

        await page.locator("#revert").click();
        await waitForHint(page, "Back to the last version");
        check(await page.locator("[data-link='audits']").count() > 0,
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
