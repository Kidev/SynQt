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
// assets are served as static files under the policy the CLI's own server sends, which is
// the policy that copy has to live under, and the page has to come up as a drawing board:
// Apply and the rest of what writes to a project gone, and Export offering both ways of
// taking the design away. Nothing may be fetched from another origin and nothing may be
// refused by the policy. The CLI has a server sending that header on every response, and the
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
//
// The synthesised drag does not always deliver its drop, which left a case waiting for an
// entity that was never made. So the drop is confirmed by the entity count and the gesture
// tried again, three times at most: a page that refuses the drop still fails, on the count.
async function dropEntity(page, label, at) {
    const before = await page.locator("#nodes [data-entity]").count();
    for (let attempt = 0; attempt < 3; ++attempt) {
        await page.locator(".palette__item", { hasText: label })
            .dragTo(page.locator("#canvas"), { targetPosition: at });
        try {
            await page.waitForFunction(
                (count) => document.querySelectorAll("#nodes [data-entity]").length > count,
                before, { timeout: 3000 });
            return;
        } catch (error) {
            // Not delivered. Try the gesture again.
        }
    }
}

// The project is read-only until it is opened for editing. The pane holds the entities' own
// code, and a stray keystroke over a file being read is not an edit. One press opens all of it, so this is a no-op once it has been pressed.
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
// The drawing board's two ways of keeping a design, both behind one button in the bar.
// Waiting on the button being there is how these cases know the page found no server behind
// it: Apply goes away with the rest of the controls that write to a project.
async function waitForDrawingBoard(page) {
    await page.waitForFunction(
        () => document.getElementById("apply").hidden
              && !document.getElementById("examples").hidden);
}

async function exportProject(page) {
    await page.locator("#export").click();
    await page.waitForSelector("#menu:not([hidden])");
    return page.locator(".menu__item", { hasText: "Export as project" }).click();
}

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

        // The token is out of the address once the page has it, so a shared screen or a
        // copied link does not carry it, and a reload still reaches the server.
        check(!page.url().includes("token="),
              "the token is taken out of the address once the page has read it");
        await page.reload();
        await page.waitForFunction(
            () => document.getElementById("project").textContent === "gavel");
        check(await page.locator("[data-entity]").count() === 3,
              "a reload still reaches the server without the token in the address");

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

        // OWNER and CONSUMER are about one link, and only ever one. `edge` is the middle of
        // a chain (it consumes the point `service` owns and owns the point `app` consumes),
        // so with one of those selected and the pointer on the other it could wear both
        // words at once, about two different links. A point that is not the selection says
        // where the pointer is and no more.
        const roleOn = (name) => page.locator(`[data-entity="${name}"]`).evaluate(
            (node) => [...node.classList].filter((one) => one.startsWith("is-")).join(" "));
        // Off the drawing between the two, because the card the pointer opens is over
        // whatever it is on, and a click aimed through it never lands.
        const offCanvas = () => page.mouse.move(5, 5);
        // Suppressed only where it should be. The selection's own line still names its two
        // ends while the pointer is on it, which is the state somebody works in.
        await page.locator('[data-contract="service"]').click();
        await page.waitForSelector('[data-entity="edge"].is-lit-consumer');
        await hoverAlong(page, "service", 0.5);
        const held = await roleOn("edge");
        check(held.includes("is-consumer"),
              `hovering the selected point still names its two ends (${held})`);
        await offCanvas();
        await page.locator('[data-contract="edge"]').click();
        await page.waitForSelector('[data-entity="edge"].is-lit-owner');
        await hoverAlong(page, "service", 0.5);
        const crossed = await roleOn("edge");
        check(!crossed.includes("is-consumer"),
              `and an entity between a selected point and a hovered one is named by the `
              + `selected one alone (${crossed})`);
        await offCanvas();

        // A member is declared on the entity, in the entity's own file, and it crosses a
        // connect point because somebody ticked it there. Those are two gestures on two
        // different things, and the panel only ever offers the one that belongs to what is
        // selected. The line into a point offers neither.
        await page.locator('[data-entity="service"]').click();
        // Annotated, which is the form the guide asks for and the form that carries a type
        // onto the contract. An unannotated parameter is `var`, because the file
        // does not say what it is.
        await typeIntoRootBlock(page, "function logWinner(winner: string) {}");

        // The contract icon is what this checks. Clicking it opens what crosses, and the
        // list it opens is ticked out of what the owner declares and nothing else.
        //
        // Said in the contract's own vocabulary rather than the QML the owner writes it in. This list
        // is the `export:` block, where a function is a `slot`. Saying `function` for a
        // member read out of the owner's file and `slot` for one that only exists on the
        // point would make a single list carry two words for the same thing.
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
                // The policy the CLI's own server sends (design.py `_CSP`), near enough:
                // everything from this origin, and a `data:` image, which is how the drawing
                // is handed to a canvas on its way to being a PNG.
                "Content-Security-Policy": "default-src 'self'; img-src 'self' data:",
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

// Start a drag off a break, look at where the half-drawn line begins, and let go without
// having wired anything. What is asserted is which end of the line the drag draws from.
async function ghostLeavesTheOwner(page, link) {
    const mark = await page.locator(`[data-break="${link}"] .link__break-disc`).boundingBox();
    // The icon itself, not the group it is in. A connect point with a finding against it
    // carries an alert mark off to one side, and the group's box is then the two of them.
    const badge = await page.locator(`[data-contract="${link}"] .link__doc-box`).boundingBox();
    const canvas = await page.locator("#canvas").boundingBox();
    const start = { x: mark.x + (mark.width / 2), y: mark.y + (mark.height / 2) };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    // Let go on empty canvas, well clear of every entity. What is being asserted is which end
    // the line is drawn from, and a drop that landed on a scope would mend the link the next
    // check is about to mend.
    await page.mouse.move(canvas.x + 40, canvas.y + canvas.height - 40, { steps: 8 });
    const from = await page.evaluate(() => {
        const line = document.querySelector("#ghost line");
        if (!line) {
            return null;
        }
        const canvas = document.getElementById("canvas");
        const point = canvas.createSVGPoint();
        point.x = Number(line.getAttribute("x1"));
        point.y = Number(line.getAttribute("y1"));
        const on = point.matrixTransform(line.getScreenCTM());
        return { x: on.x, y: on.y };
    });
    await page.mouse.up();
    if (!from) {
        return false;
    }
    // Within a couple of pixels of the middle of the icon, which is a different place from
    // the cross by most of the width of the canvas.
    return Math.hypot(from.x - (badge.x + (badge.width / 2)),
                      from.y - (badge.y + (badge.height / 2))) < 3;
}

// The break on a line into a front, dragged onto the word naming a scope. The fix for what the
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
        await waitForDrawingBoard(page);
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
        // Said by the whole line as well as by the cross on it. The cross is one mark near the
        // far end of a curve crossing the canvas, and the line is what a pointer reaching for
        // it lands on. Answering the line with the ordinary card had the drawing describe a
        // link that carries nobody as though it worked.
        await hoverAlong(page, "service", 0.3);
        await page.waitForSelector("#tip:not([hidden])");
        check((await page.locator("#tip .tip__kind").first().textContent()) === "broken",
              "and pointing anywhere along it says the same thing the cross does");
        // The travelling dash stops there too: it draws travel, and past the break nothing
        // travels.
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
        // And the line it does pull leaves the owner's own connect point, because the gesture
        // is this link being wired the way it would have been wired in the first place. Drawn
        // out of the cross, it read as the cross being a connect point of its own, halfway
        // across the canvas from the entity that owns it.
        check(await ghostLeavesTheOwner(page, "service"),
              "and dragging it draws the line from the owner's connect point, not the cross");
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
        // The 404 on api/project is what this case is built around. It is how the page finds
        // out there is nobody behind it. Chromium reports a failed fetch as a console error
        // whose text names no URL, so it is matched on where it came from, and only that
        // one is dropped. Everything else this case exists to catch is kept.
        const from = (message.location() || {}).url || "";
        if (message.type() === "error" && !from.endsWith("/api/project")) {
            refused.push(`${message.text()} (${from})`);
        }
    });
    try {
        await page.goto(`${origin}/index.html`);
        await waitForDrawingBoard(page);
        check(await page.locator("#review").isHidden(),
              "with no project to write to, Review is not offered");
        check(await page.locator("#infer").isHidden(),
              "and neither is reading contracts back out of a project that is not there");
        check(await page.locator("#apply").isHidden(),
              "and with nothing to apply a change set to, neither is Apply");
        check(!(await page.locator("#export").isHidden()),
              "taking the design away is Export's, which is the same button either way");

        // It is still an editor. The palette works and the rules paint.
        await dropEntity(page, "Client", { x: 300, y: 200 });
        await page.waitForSelector('[data-entity="client"]');
        // Under Review in the rail, the only place a finding is said: the bar carries the
        // project's name and no verdict, because a count there would be the same news with
        // nowhere to click through to.
        const findings = await page.locator("#findings").textContent();
        check(/web edge/.test(findings),
              `a client with no web edge is a problem the page paints for itself: ${findings}`);
        check(await page.locator("#verdict").count() === 0,
              "and the bar carries no verdict of its own");

        // A monitor, drawn on the copy with no scaffolder behind it. One node goes on the
        // canvas and four things come out: the entity, the console client, the sign-in gate
        // the console is hidden behind, and `monitoring.entity`. Three of those are files,
        // which is why this could not be done here at all until the scaffolder started
        // publishing them. A download carrying the entity and not them is one nothing can
        // finish, because `synqt add entity` refuses an entity that already exists.
        await dropEntity(page, "Monitor", { x: 620, y: 200 });
        await page.waitForSelector('[data-entity="ops"]');
        check(await page.locator('[data-entity="ops"]').count() === 1,
              "a monitor can be drawn here, which is the whole of what a monitor needs");
        const monitorDownload = await Promise.all([
            page.waitForEvent("download"),
            exportProject(page),
        ]);
        const written = fs.readFileSync(await monitorDownload[0].path()).toString("latin1");
        for (const wanted of ["monitor/ops/signin/index.html",
                              "client/ops-console/Main.qml"]) {
            check(written.includes(wanted), `and the download carries ${wanted}`);
        }
        // The console it carries is the real one.
        check(written.includes("ApplicationWindow") && written.length > 12000,
              `and the console in it is the real one (${written.length} bytes)`);

        // Both side panels fold to a tab against the edge of the window, and the drawing gets
        // the width. On a phone a rail, a sliver of canvas and a panel is a page showing no
        // design at all, and on a wide window it is still the way to give a big drawing the
        // room.
        const drawn = async () => (await page.locator(".stage").boundingBox()).width;
        const bothOpen = await drawn();
        await page.locator("#rail-handle").click();
        await page.waitForFunction(
            () => document.querySelector(".work").classList.contains("is-rail-shut"));
        const railShut = await drawn();
        await page.locator("#inspector-handle").click();
        await page.waitForFunction(
            () => document.querySelector(".work").classList.contains("is-panel-shut"));
        const bothShut = await drawn();
        check(railShut > bothOpen && bothShut > railShut,
              `either panel folds and the drawing takes the width (${Math.round(bothOpen)} `
              + `-> ${Math.round(railShut)} -> ${Math.round(bothShut)})`);

        // All of it, rather than all but a column each side. A fold that leaves a full-height
        // strip behind for the handle to sit on is a column of furniture either side of
        // the drawing that costs the same whether the panel is open or shut. What is left
        // is the handle itself, as a tab, shorter than the window it is against, and the thing
        // that brings the panel back.
        const work = await page.locator(".work").boundingBox();
        check(Math.round(bothShut) === Math.round(work.width),
              "and a folded panel leaves no column behind it");
        for (const which of ["rail", "inspector"]) {
            const tab = await page.locator(`#${which}-handle`).boundingBox();
            check(tab.width > 0 && tab.height < work.height / 4,
                  `the folded ${which} leaves a tab rather than a strip `
                  + `(${Math.round(tab.width)}x${Math.round(tab.height)})`);
        }

        await page.locator("#rail-handle").click();
        await page.locator("#inspector-handle").click();
        await page.waitForFunction(
            () => !document.querySelector(".work").classList.contains("is-panel-shut"));
        check(Math.round(await drawn()) === Math.round(bothOpen),
              "and the tab each one leaves behind is what brings it back");

        // Narrow enough that the three columns become one drawing with two panels over it.
        // A panel opening over the canvas leaves the grid to do it, and the two columns it is
        // not in are placed by name for exactly that reason. Left to auto-placement the stage
        // fell into the rail's column the moment the rail went absolute, took its width, and
        // the drawing went off the side of the page for as long as the rail was open.
        await page.setViewportSize({ width: 640, height: 900 });
        await page.waitForFunction(
            () => document.querySelector(".work").classList.contains("is-rail-shut"));
        const phone = (await page.locator(".work").boundingBox()).width;
        await page.locator("#rail-handle").click();
        await page.waitForFunction(
            () => !document.querySelector(".work").classList.contains("is-rail-shut"));
        check(Math.round(await drawn()) === Math.round(phone),
              "a panel opening over the drawing on a narrow window leaves the drawing the "
              + "whole width");
        await page.locator("#rail-handle").click();
        await page.waitForFunction(
            () => document.querySelector(".work").classList.contains("is-rail-shut"));
        await page.setViewportSize({ width: 1440, height: 900 });
        await page.waitForFunction(
            () => !document.querySelector(".work").classList.contains("is-rail-shut"));

        // The drawing as a picture, under the policy. The SVG on the page is carried into a
        // `data:` image with this page's stylesheet inside it, drawn into a canvas and handed
        // back as a PNG. Every step of that is something a strict policy can refuse, which is
        // why it is proven on the copy that has to live under one.
        await page.locator("#export").click();
        await page.waitForSelector("#menu:not([hidden])");
        await page.locator(".menu__item", { hasText: "Export as image" }).click();
        await page.waitForSelector("#modal[open]");
        const [picture] = await Promise.all([
            page.waitForEvent("download"),
            page.locator("#modal-yes").click(),
        ]);
        check(picture.suggestedFilename().endsWith(".png"),
              `Export hands back a picture (${picture.suggestedFilename()})`);
        const bytes = fs.readFileSync(await picture.path());
        check(bytes.length > 1000 && bytes.subarray(1, 4).toString() === "PNG",
              `and what it hands back is a real PNG (${bytes.length} bytes)`);

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
        await page.goto(`${origin}/index.html#example=demo`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 3);
        // The fragment names the example. The project it opens is called what it is. The
        // two are not the same word: `#example=demo` is the link the front
        // page publishes, and `chat` is the project under examples/ that link opens.
        check(await page.locator("#project").textContent() === "chat",
              "the fragment named a project and the page opened it");
        // One line per consumer, and one contract icon per point, so a point two entities
        // consume is two of these under one icon. Both points here have a single consumer,
        // which makes the two counts equal. The case that tells them apart is drawn further
        // down, where a second consumer is added to one of them.
        check(await page.locator("#links [data-link]").count() === 2,
              "with the connect points it declares");
        check(await page.locator("#links [data-contract]").count() === 2,
              "each drawn with a single contract icon");
        const rows = await page.locator(".palette__item").count();
        check(rows > 0 && await page.locator(".palette__glyph svg").count() === rows,
              "every palette row carries the glyph the canvas draws that entity with");
        // Every row on the rail can be drawn here, the monitor included. It could not be:
        // a monitor is four things and three of them are files, the console's being three
        // hundred lines of QML, and this copy has no scaffolder. It carries the
        // scaffolder's own templates now (assets/design/monitor.js, generated from
        // synqt/monitorscaffold.py), so drawing one is proven by drawing one rather than by
        // reading the class on a row.
        const dimmed = await page.locator(".palette__item.is-unavailable").count();
        check(dimmed === 0, `no row on the rail is refused (${dimmed} dimmed)`);
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

        // The one switch a whole project hangs off: whether this edge runs the sign-in. It
        // decides whether anybody ever leaves the default scope, so every member gate and
        // every bundle above the first depends on it, so it is readable on the canvas rather
        // than only by selecting the edge and finding a checkbox.
        check(await page.locator('[data-entity="edge"] .signin').count() === 1,
              "the edge that signs people in says so on the canvas");
        check(await page.locator('[data-entity="store"] .signin').count() === 0,
              "and nothing else carries the mark");
        // The mark takes no pointer of its own. It rides the ring of handles a link is
        // pulled out of, and one that answered for a handle would be a mark you could not
        // draw a line from. What it means is on the entity's own card.
        // On the glyph and not on the group, whose box takes in the name and the kind
        // written under it, so its middle is as often a word as the shape.
        await page.locator("#nodes [data-entity='edge'] .node__glyph").hover();
        await page.waitForSelector("#tip:not([hidden])");
        const signin = await page.locator("#tip").textContent();
        check(signin.includes("Session.login()"),
              `and the edge's card says what it means (${
                  (signin.match(/signs people in.{0,34}/s) || [""])[0].trim()})`);

        // Every link is a curve, so that two entities talking both ways, or one owning
        // several points another consumes, are lines somebody can tell apart.
        check(await page.locator("#links path.link__line").count() === 2,
              "the links are curves, not lines laid over each other");

        // What a line carries, as a block rather than as a stack of centred strings: one
        // left edge for the marks, one for the names, and a ground under both so a row
        // landing on a zone edge is still a row.
        // The browser-facing one, by the point it belongs to. Both points here carry `say`
        // and `erase`, so picking a block by a member it holds picks whichever was drawn
        // first.
        const block = page.locator("#links [data-link='edge'] .link__members").first();
        const columns = await block.locator(".link__member").evaluateAll(
            (rows) => rows.map((row) => Math.round(row.getAttribute("x") * 100)));
        check(columns.length === 3 && new Set(columns).size === 1,
              `every member of a link starts in one column (${columns.join(" ")})`);
        // Every member says the scope that gates it, on the row, in the notation the
        // `export:` block gates it in. An asterisk with the scope itself only
        // in the tooltip would make the one question a gated row raises the one thing it does
        // not answer until it is pointed at.
        const gates = await block.locator(".link__member").allTextContents();
        check(gates.every((row) => / <\w+>$/.test(row)),
              `every member names the scope that gates it (${gates.join(" | ")})`);
        // And the one worth marking is the one held above the scope the whole point is
        // behind: the room is `scope: user`, so three of these read `<user>` and say nothing
        // about the member they are on, and `<admin> slot erase(int id)` is the exception.
        const gated = await block.locator(".link__member.is-scoped").allTextContents();
        check(gated.length === 1 && gated[0].endsWith(" <admin>"),
              `and only a member above the point's own scope is marked (${gated.join(" ")})`);
        // Across both blocks, because what is being asserted is that the mark is the kind:
        // one point carries the model and the other the property, and a single block would
        // only ever prove it for the kinds that block happens to hold.
        const marks = await page.locator("#links .link__mark").evaluateAll(
            (rows) => rows.map((row) => row.dataset.kind));
        check(marks.join(" ") === "model slot slot prop slot slot",
              `each with the mark for what it is, and the word gone (${marks.join(" ")})`);
        // On the database's block, whose first row is the property. A row that said `prop`
        // as well as carrying the mark for one would be saying it twice.
        const first = page.locator("#links [data-link='store'] .link__members").first()
                          .locator(".link__member").first();
        check(!(await first.textContent()).includes("prop"),
              "so the row is the member and nothing else");
        check(await block.locator("rect.link__members-box").count() === 1,
              "over one ground the whole block sits on");
        // The word is off the canvas, so pointing at the shape is what has to say it.
        await block.locator("[data-member='messages'].link__mark").hover();
        await page.waitForSelector("#tip:not([hidden])");
        const said = await page.locator("#tip").textContent();
        check(said.includes("messages") && said.includes("model"),
              `and hovering a mark says which of the four it is (${said.slice(0, 60)})`);

        // A link, selected. The panel opens with the mark the thing was clicked on, the two
        // entities it runs between, and a line saying what that means in their own names. The
        // arrow between them is drawn, because a `>` typed between two names at this size is
        // punctuation a reader has to decide is an arrow.
        await page.locator("#links [data-contract='edge']").click();
        await page.waitForSelector(".inspector__head .ends");
        check(await page.locator(".inspector__head .ends svg.arrow").count() === 1,
              "a connect point is named by its two ends, with the arrow drawn between them");
        const ends = await page.locator(".inspector__head .ends").textContent();
        const says = await page.locator(".inspector__help").first().textContent();
        check(ends.includes("edge") && ends.includes("app")
              && says.includes("'edge'") && says.includes("'app'"),
              `and what it is, is said in those names (${says.slice(0, 56)}...)`);

        // The pane is open with the page. The files are the design itself, so they show
        // without being asked for.
        await page.waitForSelector(".tree__file");
        check(!(await page.locator("#dock").evaluate(
                  (dock) => dock.classList.contains("is-collapsed"))),
              "the files pane is open without being asked for");
        // The whole path each row stands for, which is what the row carries: directories nest,
        // and a chain with no fork in it is drawn folded onto one row.
        const folders = await page.locator(".tree__folder").evaluateAll(
            (rows) => rows.map((row) => row.dataset.folder));
        // Each entity's own folder, whole. The folder its type puts it in and then its name.
        // The database's is three segments deep and drawn as one row, which is what a chain
        // with no fork in it looks like folded.
        check(["client/app", "web/edge", "db/relational/store"]
              .every((name) => folders.includes(name)),
              `the tree is entity directories, not one flat list (${folders.join(" ")})`);
        const named = await page.locator(".tree__file").allTextContents();
        // Every entity present, with its own file, and the project's own configuration above
        // them all.
        const wanted = ["synqt.yaml", "Main.qml", "Edge.qml", "Store.qml", "schema.sql"];
        const missing = wanted.filter((name) => !named.includes(name));
        check(missing.length === 0,
              missing.length ? `the files pane is missing ${missing.join(", ")}; it names `
                               + named.join(", ")
                             : "every entity has its own file, and every file its directory");

        await fileRow(page, "web/edge/Edge.qml").click();
        const source = await sourceText(page);
        // Rooted at its own name: `edge` exports `Edge`, and `Edge {}` is the Source that
        // answers it. One entity, one file, one name.
        check(source.includes("Edge {"),
              "and reading one shows the Source the owner would host");
        check(!source.includes("SPDX-License-Identifier"),
              "without the licence notice, which is on every file and read by nobody");
        check(await page.locator(".cm-content .tok--keyword").count() > 0,
              "coloured as the QML it is");

        // Opening a file selects what it is on the canvas, and the reverse, so the two views
        // never disagree about what is in hand. The entity. A file sits in an entity's folder
        // and is that entity's own code, whether or not a connect point is exported out of it.
        await page.waitForSelector("[data-entity='edge'].is-selected");
        check(true, "opening a file selects the entity it belongs to, out on the canvas");
        await page.locator("#nodes [data-entity='app']").click();
        await page.waitForFunction(
            () => document.getElementById("source-name").textContent
                === "client/app/Main.qml");
        check(true, "and selecting an entity opens the file it is");
        // That click was a redraw, with the pointer still on the entity it drew. The handles
        // a link is pulled from are put on whichever entity the pointer is nearest, and the
        // drawing is rebuilt from the document on every change. If they went with it, a
        // press where a handle had been a moment ago would land on the canvas behind it and
        // pan the view instead of starting a link.
        check(await page.locator("[data-entity='app'].is-near").count() === 1,
              "and the handles a link is drawn from survive the redraw that click causes");

        // Read-only until it is opened for editing. The pane holds the entities' own code.
        await fileRow(page, "web/edge/Edge.qml").click();
        check(await sourceIsLocked(page), "a file opens read-only");
        // And what the button opens is the project, not the file on screen when it was
        // pressed. Following a declaration from one entity into another is three files in a
        // minute, and a lock per file would interrupt that three times.
        await unlock(page);
        await fileRow(page, "synqt.yaml").click();
        const alsoOpen = !(await sourceIsLocked(page));
        await fileRow(page, "web/edge/Edge.qml").click();
        check(alsoOpen && !(await sourceIsLocked(page)),
              "and Edit files opens every one of them, not the one that was on screen");
        // Pressed again it shuts all of them, which is what makes it a mode somebody can
        // leave rather than a door propped open for the rest of the afternoon.
        await page.locator("#source-lock").click();
        const shut = await sourceIsLocked(page);
        await fileRow(page, "synqt.yaml").click();
        check(shut && await sourceIsLocked(page), "and pressing it again closes them all");
        await fileRow(page, "web/edge/Edge.qml").click();
        await unlock(page);
        // Typing a declaration into a Source is the same gesture as adding a member in the
        // panel, which is the whole reason the pane is a textarea and not a preview. It
        // declares. It does not export. A contract is the list of what an owner has agreed to
        // say to somebody else, and writing a property on an entity is not that agreement.
        // It is also how a half-typed name would walk onto the wire one letter at a time.
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
        // And ticking it is what puts it there. The contract icon opens the point. The list
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

        // Backspace over a file being typed into is a character, never the entity whose
        // file it is. The page's answer to who has focus stops at a shadow host, and the
        // pane is an editor inside one, so a page-level check sees the plain <div> the
        // editor is built into, not a field, and the canvas would delete the selected
        // entity. Typed at the end of the file, where there is something to erase.
        const held = await page.locator("#nodes [data-entity]").count();
        await clickIntoSource(page);
        await page.keyboard.press("Control+End");
        await page.keyboard.type("xy");
        await page.keyboard.press("Backspace");
        await page.keyboard.press("Backspace");
        await page.waitForTimeout(150);
        check(await page.locator("#nodes [data-entity]").count() === held
              && !(await sourceText(page)).endsWith("xy"),
              "and Backspace there erases a character, not the entity the file belongs to");

        // Reaching into another entity puts the member on the connect point it would cross.
        // This is what `synqt infer` does over a project, done here on one file while it is
        // being typed. `Server` is the client's alias for the edge, so the edge's point is
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
        // the exception. Code that is written is somebody having said so, and asking them to
        // tick a box for a call they have already made is asking them to say it twice.
        // The edge's file calls `Store.say(...)`, so drawing that link back gives it back.
        await page.locator("[data-contract='store']").click();
        await page.locator(".inspector__actions .button--danger",
                           { hasText: "Delete connect point" }).click();
        await page.waitForFunction(
            () => document.querySelectorAll("[data-contract='store']").length === 0);
        await dragLink(page, "store", "edge");
        await page.waitForSelector("[data-contract='store']");
        await openAndWaitFor(page, "synqt.yaml", "owner: store");
        check((await sourceText(page)).includes("slot say("),
              "a link drawn to a consumer that already calls into it carries what it calls");
        // Shut the ticker the new link opened. It is put beside the line it is about, so it
        // covers a piece of canvas, and what is under it here is the entity the next drag
        // starts from. A press that lands on it draws nothing and reports nothing.
        await page.locator(".picker__foot .button", { hasText: "Done" }).click();
        await page.waitForFunction(() => document.getElementById("picker").hidden);

        // The pane is an editor and not a box with text in it, and the difference is a list
        // of things a reader can point at. The one this asserts is the one everything else
        // relies on. The file's lines are numbered, so a finding, a stack trace or a
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
        // chevron rebuilt as part of closing detaches the element the click landed on; the
        // click then carries on to the strip, finds no button above it, takes itself for a
        // click on the strip and opens the pane again in the same turn. That fails only
        // every other press, which one press never catches.
        await page.click("#dock-toggle");
        check(await page.locator("#dock").evaluate(
                  (dock) => dock.classList.contains("is-collapsed")),
              "hiding the pane leaves the bar it opens again from");
        check(await page.locator("#dock-bar").isVisible()
              && await page.locator("#dock-pane").isHidden(),
              "with the pane itself gone");
        // What is left on it is what a collapsed pane is for. The word that says what is
        // behind the strip, and the one control that brings it back. The file's name and the
        // button that unlocks it go with the pane, because a name with nothing under it is a
        // file nobody can see and Edit over a pane that is not there unlocks a file for a
        // caret with nowhere to go.
        check(await page.locator(".dock__title").isVisible()
              && await page.locator("#dock-toggle").isVisible()
              && await page.locator("#source-name").isHidden()
              && await page.locator("#source-lock").isHidden(),
              "carrying only Files and the control that opens it again");

        // Folded, that bar is the last inch of the page, which on a phone is the one place
        // a page measured in `vh` cannot put anything. `100vh` is the window with the
        // browser's own bars retracted, so a body exactly that tall, hiding its overflow,
        // ends below the screen with nothing to scroll to, and the bar (the word saying
        // what is behind it and the only control that brings it back) would be below the
        // screen. The page is measured in `dvh`, the window as it stands.
        await page.setViewportSize({ width: 390, height: 844 });
        check(await page.evaluate(() => {
                  for (const sheet of document.styleSheets) {
                      let rules;
                      try {
                          rules = sheet.cssRules;
                      } catch (error) {
                          continue;       // another origin's sheet, and not one of the page's
                      }
                      for (const rule of rules) {
                          if (rule.selectorText === "body") {
                              return rule.style.height === "100dvh";
                          }
                      }
                  }
                  return false;
              }),
              "the page is as tall as the window is, not as tall as it would be with the "
              + "browser's own bars out of the way");
        const folded = await page.locator("#dock-bar").boundingBox();
        check(folded.height > 0 && folded.y + folded.height <= 844
              && await page.locator("#dock-toggle").isVisible()
              && await page.locator(".dock__title").isVisible(),
              "so the folded bar is on screen on a phone, control and all "
              + `(bottom ${Math.round(folded.y + folded.height)} of 844)`);
        await page.setViewportSize({ width: 1440, height: 900 });
        await page.waitForFunction(
            () => !document.querySelector(".work").classList.contains("is-rail-shut"));

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
        // Which is the case the two counts at the top of this could not tell apart: two
        // lines now, and the one icon they both leave from underneath. The icon is the
        // point, whatever its consumer list holds, and every line into it starts there.
        check(await page.locator("[data-contract='store']").count() === 1,
              "still drawn with a single contract icon, not one per consumer");

        // Delete removes what is selected, and a double click renames it.
        await page.locator("#nodes [data-entity='cache']").click();
        await page.locator("#canvas").press("Delete");
        await page.waitForFunction(
            () => !document.querySelector("#nodes [data-entity='cache']"));
        check(true, "Delete removes what is selected");

        // And it can be taken back. Everything an edit changes is in the document, so a step
        // back is the document as it was. The entity returns with the connect point it was a
        // consumer of, which is the half of a delete that is easy to miss and impossible to
        // put back by hand.
        check(!(await page.locator("#undo").isDisabled()),
              "with something to undo, the way back is offered");
        // At the head of the controls on the right, not beside the project's name: everything
        // at that end of the bar is something to press, and these two are the first thing
        // reached for when the last thing pressed was wrong.
        const stepBox = await page.locator("#undo").boundingBox();
        const namedBox = await page.locator("#project").boundingBox();
        // Measured against Export rather than Apply, because this is the drawing board and
        // Apply is not on it: Export is the button at that end here, and the two steps are
        // still meant to come before it.
        const exportBox = await page.locator("#export").boundingBox();
        check(stepBox.x > namedBox.x + namedBox.width && stepBox.x < exportBox.x,
              "and it sits with the buttons at that end of the bar, at the head of them");
        await page.locator("#undo").click();
        await page.waitForSelector("#nodes [data-entity='cache']");
        check(await page.locator("[data-link='store']").count() === 2,
              "and undo brings back what was deleted, lines and all");
        await page.locator("#redo").click();
        await page.waitForFunction(
            () => !document.querySelector("#nodes [data-entity='cache']"));
        check(await page.locator("#redo").isDisabled(),
              "redo takes it away again, and then there is nothing ahead");

        // The panel says what each of its controls is behind a `?` beside that control, and
        // not in a paragraph under every one of them. Eleven settings with an explanation
        // before each is more prose than settings, and a reader looking for the one they came
        // for reads all of it on the way past. On the setting, not gathered onto the section:
        // the answer wanted is the answer about the thing being pointed at.
        await page.locator("#nodes [data-entity='store']").click();
        const runs = page.locator(".block").filter({ hasText: "How it runs" }).first();
        const ask = runs.locator(".field .ask").first();
        await ask.waitFor();
        check(!(await runs.locator("> .field__note").count())
              && await ask.evaluate((mark) => getComputedStyle(mark).opacity === "0"),
              "a control's explanation is behind a ? beside it, out of sight until asked");
        await runs.hover();
        // Handed the element itself, because every control that explains itself has one of
        // these and the first in the document is not necessarily this one.
        await page.waitForFunction((mark) => getComputedStyle(mark).opacity !== "0",
                                   await ask.elementHandle());
        check(true, "which appears the moment the pointer is on the section it belongs to");
        await ask.hover();
        const aside = runs.locator(".field .ask__tip").first();
        await page.waitForFunction((words) => getComputedStyle(words).visibility === "visible",
                                   await aside.elementHandle());
        const behind = (await aside.textContent()).trim();
        check(behind.includes("engine behind this kind of entity"),
              `and hovering it is what says it (${behind.slice(0, 48)}...)`);
        // The section keeps a mark of its own, for what the group of settings is. Both, so
        // neither question has to be asked of the other one's answer.
        check(await runs.locator(".block__head .ask").count() === 1
              && await runs.locator(".field .ask").count() > 0,
              "the section says what it is for, and each setting in it says what it is");

        // A member of a contract is a line of code, and is painted as one wherever it is
        // written out. The same three colours the file pane uses, so the panel and the file
        // are one contract and not two spellings of it.
        await page.locator("#nodes [data-entity='edge']").click();
        const declared = page.locator(".members .member__summary");
        await declared.first().waitFor();
        // Across the list rather than on its first row. What a row holds depends on what it
        // declares, and a function that takes nothing and answers nothing is a name and two
        // brackets. Every row has a word for what it is. A type is on whichever rows have one.
        check(await declared.locator(".code__tok--kw").count() === await declared.count()
              && await declared.locator(".code__tok--type").count() > 0,
              "what an entity declares is painted the way the file that declares it is");

        // In place, over the entity, and not in a dialog. The field opens where the name was,
        // holding it, and Enter is what commits. A prompt would cover the drawing the new
        // name is being chosen against, which is the only thing anybody is looking at.
        await page.locator("#nodes [data-entity='app']").dblclick();
        await page.waitForSelector(".rename");
        check(await page.locator(".rename").inputValue() === "app",
              "a double click opens the name where the name is");
        await page.locator(".rename").fill("lobby");
        await page.locator(".rename").press("Enter");
        await page.waitForSelector("[data-entity='lobby']");
        check(await page.locator("#nodes [data-entity='app']").count() === 0
              && await page.locator(".rename").count() === 0,
              "and typing a new one there renames it");

        // Who is at the other end of every line this entity is on, on the panel, as something
        // to press. The panel is where a reader ends up after clicking one thing, and the
        // next thing they want is usually at the other end of a line: reading "owner: store"
        // and then having to find `store` on the canvas is the panel naming a thing it will
        // not take you to.
        await page.locator("#nodes [data-entity='store']").click();
        await page.waitForSelector("[data-entity='store'].is-selected");
        const wired = await page.locator(".inspector__body .chips .button--chip")
                                .allTextContents();
        check(wired.includes("edge"),
              `the panel names the entities at the other end of its lines (${wired.join(", ")})`);
        await page.locator(".inspector__body .chips .button--chip", { hasText: "edge" })
                  .first().click();
        await page.waitForSelector("[data-entity='edge'].is-selected");
        check(true, "and pressing one of them is how you get there");

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
        // thing at the other end means. The browser is the only consumer of the point the
        // edge exports, so that point goes with it.
        //
        // Opened with the pointer at the middle of the disc rather than through a locator:
        // a node is drawn in layers, so every element in it but the innermost mark on the
        // glyph has its own middle covered by the one in front, and that mark is not what
        // anybody aims at.
        const middle = await discCentre(page, "lobby");
        await page.mouse.click(middle.x, middle.y, { button: "right" });
        await page.waitForSelector(".menu__item");
        check(await page.locator(".menu__what").textContent() === "lobby",
              "a right click opens a menu over what it was opened on");
        await page.locator(".menu__item", { hasText: "Delete" }).click();
        await page.waitForFunction(
            () => !document.querySelector("#nodes [data-entity='lobby']"));
        check(await page.locator("#nodes [data-entity]").count() === 2,
              "and Delete there removes it");
        // One line. The database's point, which the edge still consumes. It survives because
        // it never ran to or from the browser. The one that did went with it.
        const left = await page.locator("#links [data-link]").count();
        check(left === 1,
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
        await page.goto(`${origin}/index.html#example=demo`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 3);

        // Declaring on an entity, from the panel. This is the pool every connect point the
        // entity owns ticks its contract from, and it is reachable from the panel rather than only by typing the
        // line into the file. A free-text prompt on a link and nothing at
        // all on an entity would leave it there.
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
        // list. The type is a drop-down over the contract vocabulary. The name is the one
        // part that has to be typed, because nothing could offer it. Both edits go back into
        // the line they were read from.
        //
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
        const standing = await page.locator("#nodes [data-entity]").count();
        check(standing === 3,
              `a line that does not read leaves the canvas on the last one that did `
              + `(${standing} entities still drawn)`);

        await page.locator("#revert").click();
        await waitForHint(page, "Back to the last version");
        check(await page.locator("[data-link='store']").count() > before,
              "and the way back returns the last version that read, keeping the work");

        // The project's own name is in that file too, and it is the one thing read out of it
        // that is not on the canvas. It is in the bar and in the tab's title. If the only writer of
        // them were the one that runs when a document arrives, both would go
        // on saying what the project was called before the edit.
        await clickIntoSource(page);
        await page.keyboard.press("Control+Home");
        await page.keyboard.press("ArrowDown");
        await page.keyboard.press("End");
        await page.keyboard.type("ing");
        await page.waitForFunction(
            () => document.getElementById("project").textContent === "chating");
        check((await page.title()) === "SynQt - chating",
              `and the tab it is open in says so too (${await page.title()})`);

        // Clearing a design that grew out of an example leaves the example as well. The
        // address is what the page reads on the way in, so a Clear that left `example=` in it
        // emptied the canvas and handed the same example straight back on the next reload,
        // which reads as a Clear that did not take.
        // Asked in the page's own face, over the drawing the question is about. The
        // browser's own box is kept for leaving the site, where the browser is the one
        // asking. Anything a listener here can hold open is asked here.
        await page.locator("#restart").click();
        await page.waitForSelector("#modal[open]");
        check(await page.locator("#modal-title").textContent() === "Clear this design?",
              "Clear asks in this page's own dialog, not in the browser's");
        await page.locator("#modal-yes").click();
        await waitForHint(page, "Cleared");
        check(!page.url().includes("example="),
              `Clear takes the example out of the address with it (${page.url()})`);
        await page.reload();
        await waitForDrawingBoard(page);
        check(await page.locator("#nodes [data-entity]").count() === 0,
              "so a reload finds the canvas it was cleared to, not the example again");

        check(refused.length === 0,
              `the policy refuses nothing on the page (${refused.join(" | ") || "no errors"})`);
    } finally {
        await browser.close();
        server.close();
    }
}

// A size on a contract member is the owner-side bound on what a caller may send, and QML has
// no sized type to declare it with. Typing into the owner's Source corrects each carried
// member from the file, and the bound has to survive that.
async function aBoundSurvivesTypingIntoTheSource() {
    console.log("\nA contract's sizes, after typing into the owner's Source");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    try {
        await page.goto(`${origin}/index.html#example=demo`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 3);
        await fileRow(page, "synqt.yaml").click();
        await page.waitForFunction(`${SOURCE}.includes("string[280]")`);
        check(true, "the example's contract carries a sized parameter");

        await openAndWaitFor(page, "web/edge/Edge.qml", "function say(");
        await typeIntoRootBlock(page, "    property int seen: 0\n");
        await waitForHint(page, "'seen' is declared on 'edge'");
        await fileRow(page, "synqt.yaml").click();
        await page.waitForFunction(`${SOURCE}.includes("slot say(")`);
        const config = await sourceText(page);
        const say = config.split("\n").find((line) => line.includes("slot say(")) || "";
        check(say.includes("string[280]"),
              `the owner-side bound is still on the contract (${say.trim()})`);
    } finally {
        await browser.close();
        server.close();
    }
}

// A link that names an example arrives over whatever this browser kept from the last visit.
// A name that is not an example, or that only resolves through Object.prototype, leaves the
// kept work alone, and a different example asks before replacing it.
async function aLinkNeverDropsKeptWork() {
    console.log("\nA link to an example, over work this browser kept");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const entities = () => page.locator("#nodes [data-entity]").count();
    try {
        await page.goto(`${origin}/index.html#example=demo`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 3);
        await dropEntity(page, "Jobs", { x: 430, y: 60 });
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 4);

        for (const name of ["nosuchexample", "__proto__", "constructor"]) {
            await page.goto(`${origin}/index.html#example=${name}`);
            await page.reload();
            await waitForDrawingBoard(page);
            await page.waitForFunction(
                () => document.querySelectorAll("#nodes [data-entity]").length > 0,
                null, { timeout: 15000 }).catch(() => {});
            const drawn = await entities();
            check(drawn === 4, `a link naming '${name}' keeps the four entities (${drawn})`);
        }

        await page.goto(`${origin}/index.html#example=gavel`);
        await page.reload();
        await page.waitForSelector("#modal[open]");
        check((await page.locator("#modal-title").textContent()).includes("gavel"),
              "a link to a different example asks before replacing the kept design");
        await page.locator("#modal-no").click();
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 4);
        check(page.url().includes("example=demo"),
              `and declining keeps it, with the address naming what is on screen (${page.url()})`);
    } finally {
        await browser.close();
        server.close();
    }
}

// A declaration added, renamed or removed from the panel is written into the entity's file,
// the same as typing it into the pane, so Apply writes it to the project on disk.
async function aPanelDeclarationReachesTheDisk() {
    console.log("\nDeclaring from the panel, then applying");
    const project = await copyProject();
    const { proc, url } = await startEditor(project);
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    try {
        await page.goto(url);
        await page.waitForFunction(
            () => document.getElementById("project").textContent === "gavel");
        await page.locator("#nodes [data-entity='books']").click();
        const declares = page.locator(".members").filter(
            { hasText: "What this entity declares" });
        await declares.getByRole("button", { name: "property", exact: true }).click();
        const row = declares.locator(".member.is-open").first();
        await row.locator("input[type=text]").fill("shelfCount");
        await openAndWaitFor(page, "db/relational/books/Books.qml", "shelfCount");

        await page.locator("#review").click();
        await page.waitForSelector("#sheet:not([hidden])");
        await page.waitForSelector("#apply:not([disabled])");
        await page.locator("#apply").click();
        await waitForHint(page, "Applied");
        const written = await fsp.readFile(
            path.join(project, "db/relational/books/Books.qml"), "utf8");
        check(written.includes("shelfCount"),
              "the property declared from the panel is in the file on disk");
    } finally {
        await browser.close();
        proc.kill();
    }
}

// Two owners can declare members of one name. Removing one from its owner takes it off that
// owner's contract and no other.
async function aRemovalStaysWithItsOwner() {
    console.log("\nRemoving a declaration two owners share by name");
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    try {
        await page.goto(`${origin}/index.html#example=demo`);
        await page.waitForFunction(
            () => document.querySelectorAll("#nodes [data-entity]").length === 3);
        await page.locator("#nodes [data-entity='edge']").click();
        await page.locator(".members").filter({ hasText: "What this entity declares" })
            .locator("button[title='Remove say']").click();
        await fileRow(page, "synqt.yaml").click();
        await page.waitForFunction(`${SOURCE}.includes("owner: store")`);
        const config = await sourceText(page);
        const points = config.split(/^connect_points:/m)[1] || "";
        const edge = points.split(/^  - owner: /m).find((one) => one.startsWith("edge")) || "";
        const store = points.split(/^  - owner: /m).find((one) => one.startsWith("store")) || "";
        check(!edge.includes("say("), "the removal took 'say' off edge's contract");
        check(store.includes("slot say("),
              `and left store's own 'say' where it was:\n${store}`);
    } finally {
        await browser.close();
        server.close();
    }
}

// Each part on its own, so one that stops is one failure and not the end of the run. The
// five are five browsers over five pages and share nothing, so what the third one does says
// nothing about the fifth, and an exception thrown out of the third must not report that as
// though it did, or the two parts behind it go unmeasured.
//
// The whole error, indented, because a Playwright timeout carries its call log in the
// message and that log is the answer to why it timed out.
// The project's scope vocabulary, which is the one setting in the editor that belongs to no
// entity and no link. It gets a part of its own because it matters twice over. The
// order is the authority ranking under `scopes.hierarchical`, and since the mapping hook
// started answering with a generated enum it is that enum's member values, so moving a row
// renumbers the vocabulary every hook in the project is written against.
async function theProjectsScopes() {
    console.log("\nThe scopes a project declares");
    const project = await copyProject();
    const { proc, url } = await startEditor(project);
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const problems = [];
    page.on("pageerror", (error) => problems.push(String(error)));
    try {
        await page.goto(url);
        await page.waitForFunction(
            () => document.getElementById("project").textContent === "gavel");

        // The panel with nothing picked is where it lives. A scope is not an entity and not
        // a link, and every gate in every other panel chooses from this list.
        const rows = page.locator(".scopes__row");
        await rows.first().waitFor();
        check(await rows.count() === 4,
              "the four scopes gavel declares are each a row to edit");

        // Renaming is typed in place, and it renames the vocabulary and nothing else. gavel
        // gates its `place` slot on `user`, so renaming that scope leaves the gate naming a
        // scope the project no longer declares, and the sheet is where that is said.
        const second = rows.nth(1).locator("input[type=text]");
        check(await second.inputValue() === "user", "the second scope is 'user'");
        await second.fill("bidder");
        await page.waitForTimeout(150);

        // Add one, and rank it. The new scope goes on the end, which is the highest
        // authority in a hierarchical project, and the arrows are what move it.
        await page.getByRole("button", { name: "Add scope" }).click();
        await page.waitForFunction(() => document.querySelectorAll(".scopes__row").length === 5);

        await page.locator("#review").click();
        await page.waitForSelector("#sheet:not([hidden])");
        const diff = await page.locator("#sheet-diff").textContent();
        check(diff.includes("synqt.yaml"),
              "editing the vocabulary is a change to synqt.yaml and to nothing else");
        check(!/map\.qml/.test(diff),
              `and not to the mapping hook, which nothing here guessed at:\n${diff}`);

        // The refusal, which is the whole design. Nothing infers that a row was typed over
        // rather than one scope removed and another added, so the uses keep naming `user`
        // and every one of them is named here before there is anything to apply.
        const refused = await page.locator("#sheet-findings").textContent();
        check(/error:/.test(refused) && refused.includes("'user'"),
              `the sheet names what is still gated on the scope that was renamed away, `
              + `and said: ${refused}`);
        check(await page.locator("#apply").isDisabled(),
              "and Apply is refused while it is");

        // Typing the name back is the way out, and it puts the project back where it was, so
        // what is left to apply is the added scope alone.
        await page.locator("#sheet-close").click();
        await second.fill("user");
        await page.waitForTimeout(150);
        await page.locator("#review").click();
        await page.waitForSelector("#sheet:not([hidden])");
        const findings = await page.locator("#sheet-findings").textContent();
        check(!/error:/.test(findings || ""),
              `the change set is one the project accepts, and said: ${findings}`);
        await page.waitForSelector("#apply:not([disabled])");
        await page.locator("#apply").click();
        await waitForHint(page, "Applied");

        const config = await fsp.readFile(path.join(project, "synqt.yaml"), "utf8");
        check(/order:\s*\[anonymous, user, moderator, admin, scope\]/.test(config),
              `the added scope is in the order:\n${config.split("\n")
                  .filter((line) => line.includes("order:")).join("\n")}`);
        check(/<user>/.test(config),
              "and the gate that was there all along is untouched");
        // The file the editor never reaches, and now never tries to. The mapping hook is a
        // hand-written file, and an edit of the vocabulary is not an edit of it.
        const hook = await fsp.readFile(path.join(project, "web/edge/identity/map.qml"),
                                        "utf8");
        check(hook.includes("Scope.User") && !hook.includes("Bidder"),
              `the mapping hook answers with the member it always did:\n${hook}`);
        check(problems.length === 0,
              `the page reported no errors, and said: ${problems.join(" | ")}`);
    } finally {
        await browser.close();
        proc.kill("SIGKILL");
    }
}

// Hover the middle of what `selector` finds, arriving from a little way off. A single jump
// onto the spot sends no pointermove to the drawing, which is what the card answers.
async function hoverOn(page, selector, dx = 0, dy = 0) {
    const box = await page.locator(selector).first().boundingBox();
    if (!box) {
        throw new Error(`nothing on the page matches ${selector}`);
    }
    const x = box.x + (box.width / 2) + dx;
    const y = box.y + (box.height / 2) + dy;
    await page.mouse.move(x - 30, y + 30);
    await page.mouse.move(x, y, { steps: 6 });
    await page.waitForTimeout(250);
}

async function cardText(page) {
    const card = page.locator(".tip");
    return (await card.isVisible()) ? (await card.textContent()) : "";
}

// The chat room, drawn by the copy the site publishes, for the cases below.
async function onTheChatRoom(what, run) {
    console.log(`\n${what}`);
    const server = await serveAssets();
    const origin = `http://127.0.0.1:${server.address().port}`;
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
    const refused = [];
    page.on("pageerror", (error) => refused.push(String(error)));
    try {
        await page.goto(`${origin}/index.html#example=demo`);
        await waitForDrawingBoard(page);
        await page.waitForSelector('[data-entity="edge"]');
        await run(page);
        check(refused.length === 0, `the page reported no errors ${refused.join(" | ")}`);
    } finally {
        await browser.close();
        server.close();
    }
}

// A monitor has no drawn links, so it is a square with no handles on its rim, in a box of its
// own outside the mesh (inside the mesh box it read as an edge nobody watched), and what it
// opens is its console.
async function theMonitorStandsApart() {
    await onTheChatRoom("A monitor, drawn apart from the mesh", async (page) => {
        await dropEntity(page, "Monitor", { x: 900, y: 430 });
        await page.waitForSelector('[data-entity="ops"]');
        const monitor = page.locator('[data-entity="ops"]');
        check(await monitor.locator(".node__square").count() === 1,
              "a monitor is drawn as a square");
        check(await monitor.locator(".node__slot-grab, .node__slot").count() === 0,
              "with no handle on it to pull a line from");
        check(/ops-console\/Main/.test(await monitor.locator(".node__file").textContent()),
              "and its console written under it");
        check(!(await monitor.locator(".node__file").isVisible()),
              "which is hidden until the monitor is hovered");
        check(await page.locator(".zone--ops .zone__title").count() === 1
              && await page.locator(".zone--ops .zone__title").textContent()
                 === "Watches every entity",
              "in a box of its own");
        const inside = String(await page.locator(".zone--ops").getAttribute("data-inside"))
            .split(" ");
        check(["ops", "app", "edge", "store"].every((name) => inside.includes(name)),
              "which holds every entity");
        const outer = await page.locator(".zone--ops .zone__box").boundingBox();
        const boxes = await page.locator(".zone:not(.zone--ops) .zone__box").evaluateAll(
            (all) => all.map((one) => one.getBoundingClientRect().toJSON()));
        check(boxes.length > 0 && boxes.every((box) => box.left >= outer.x
                                                  && box.top >= outer.y
                                                  && box.right <= outer.x + outer.width
                                                  && box.bottom <= outer.y + outer.height),
              "and is drawn round every other box");
        check(await page.locator(".zone--ops .zone__box").evaluate(
                  (box) => getComputedStyle(box).fill) === "none",
              "with no fill, so the boxes inside keep their own colours");
        const square = await discCentre(page, "ops");
        await page.mouse.click(square.x, square.y);
        await page.waitForTimeout(300);
        check(/ops-console\/Main\.qml$/.test(await page.locator("#source-name").textContent()),
              "selecting it opens the console rather than the sign-in page");
        check(!/What this entity declares/i.test(await page.locator("#inspector").textContent()),
              "and its panel offers nothing to declare on a Source that is the framework's");
        await dragLink(page, "store", "ops");
        await waitForHint(page, "'ops' is a monitor");
        check(true, "a line dropped on it is refused, and says why");
    });
}

// Every box's name is a handle, the mesh's included, and an entity lists its files only
// while it is hovered.
async function theBoxesAndFilesAnswerAlike() {
    await onTheChatRoom("Every box answers alike, and files show on hover", async (page) => {
        const cursors = await page.locator(".zone__title").evaluateAll(
            (all) => all.map((one) => getComputedStyle(one).cursor));
        check(cursors.length === 3 && cursors.every((one) => one === "grab"),
              `every box's name is a handle (${cursors.join(", ")})`);
        const title = await page.locator('[data-zone-title="mesh"]').boundingBox();
        const before = await discCentre(page, "store");
        await page.mouse.move(title.x + 10, title.y + (title.height / 2));
        await page.mouse.down();
        await page.mouse.move(title.x + 90, title.y + 60, { steps: 6 });
        await page.mouse.up();
        const after = await discCentre(page, "store");
        check(after.x - before.x > 40 && after.y - before.y > 20,
              "dragging the mesh's name moves what is in the mesh");
        const files = page.locator('[data-entity="app"] .node__file');
        check(!(await files.isVisible()), "the client's files are hidden at rest");
        await hoverOn(page, '[data-entity="app"] .node__disc');
        const listed = await files.locator("tspan").allTextContents();
        check(await files.isVisible()
              && ["client/app/", "Main.qml", "User.qml", "Message.qml", "Admin.qml"]
                  .every((name) => listed.includes(name)),
              `hovering it lists every file in its folder (${listed.join(" ")})`);
        const chips = await page.locator(".tip .tip__files .tip__ident").allTextContents();
        check(chips.includes("client/app/Message.qml") && chips.length === 4,
              "and its card names each of them");
        await page.mouse.move(5, 5);
        await hoverOn(page, 'text.link__member[data-member="messages"]');
        check(await page.locator(".tip .tip__ident .tip__tok--name").count() >= 4,
              "a member's card writes each role as the typed name it is");
    });
}

// The mark on an edge that signs people in has a card of its own, and the line from that edge
// to a browser says where `Session.login()` goes while the edge or the mark is hovered.
async function theSignInIsExplained() {
    await onTheChatRoom("The sign-in, explained where it is drawn", async (page) => {
        await hoverOn(page, '[data-entity="edge"] .signin');
        const signIn = await cardText(page);
        check(/Sign-in/.test(signIn) && /Session\.login\(\)/.test(signIn),
              "the mark on the edge opens a card of its own");
        check(await page.locator(".link.is-signin .link__signin").count() === 1,
              "and shows the sign-in on the edge's line to the client");
        await page.mouse.move(5, 5);
        await page.waitForTimeout(200);
        check(await page.locator(".link.is-signin").count() === 0,
              "which goes again when the pointer leaves");
        await hoverOn(page, '[data-entity="edge"] .node__disc', 0, 12);
        check(await page.locator(".link.is-signin .link__signin").count() === 1,
              "hovering the edge itself shows it too");
    });
}

// A member's scope is set apart in its card as the `<scope>` the row writes, rather than as a
// quoted word in a sentence.
async function theScopeIsWrittenAsTheRowWritesIt() {
    await onTheChatRoom("A member's scope, in its card", async (page) => {
        await hoverOn(page, 'text.link__member[data-member="erase"]');
        check(await page.locator(".tip .tip__scope.is-scoped").count() === 1
              && await page.locator(".tip .tip__scope.is-scoped").textContent() === "<admin>",
              "a member held above its point's scope shows it as the <admin> the row writes");
        await page.mouse.move(5, 5);
        await hoverOn(page, 'text.link__member[data-member="say"]');
        check(await page.locator(".tip .tip__scope").count() > 0
              && await page.locator(".tip .tip__scope").first().textContent() === "<user>",
              "and a member at the point's own scope shows that one, <user>");
        await page.mouse.move(5, 5);
        await hoverOn(page, '[data-contract="edge"]');
        check(await page.locator(".tip .tip__help--key .tip__scope").count() === 1,
              "the contract's card says what a <scope> after a member means");
    });
}

async function part(run) {
    try {
        await run();
    } catch (error) {
        const stopped = `${run.name} stopped before it finished`;
        console.log(`  FAIL  ${stopped}`);
        for (const line of String(error.stack || error).split("\n")) {
            console.log(`        ${line}`);
        }
        failures.push(stopped);
    }
}

await part(editorOverAProject);
await part(theCopyOnTheSite);
await part(theProjectALinkHandsYou);
await part(theFrontThatSplitsCallers);
await part(typingIntoTheProject);
await part(theProjectsScopes);
await part(theMonitorStandsApart);
await part(theSignInIsExplained);
await part(theScopeIsWrittenAsTheRowWritesIt);
await part(theBoxesAndFilesAnswerAlike);
await part(aBoundSurvivesTypingIntoTheSource);
await part(aLinkNeverDropsKeptWork);
await part(aPanelDeclarationReachesTheDisk);
await part(aRemovalStaysWithItsOwner);

console.log("");
if (failures.length) {
    console.log(`designer: ${failures.length} check(s) failed`);
    for (const what of failures) {
        console.log(`  FAIL  ${what}`);
    }
    process.exit(1);
}
console.log("designer: the editor writes what it draws, and the hosted copy stands alone");
