// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The design editor, in a real browser, both ways it is reached.
//
// Case 1 is the editor over a project: `synqt design` on a copy of the gavel example, and a
// browser that adds a service, drags a connect point from it to the web edge, names the
// point and its contract, says a slot crosses it, reviews the change set and applies it. The
// verdict is on disk afterwards, in the two files the project is made of: synqt.yaml and the
// contract under shared/. Everything between the click and the file is what this covers, and
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
// Chromium only, deliberately. What is being proven here is this page's own behaviour, not
// an engine's: the transport proofs that are per-engine questions are tests/m0-transport and
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
// build/ and .synqt/ are left behind: the first is somebody's last build and the second
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
        // -u: the URL is printed and then the server blocks in serve_forever, so a buffered
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

// A connect point is drawn from the owner's rim to the consumer's disc, and the direction is
// its meaning, so this is the one interaction the page has that a keyboard cannot reach.
async function dragLink(page, fromEntity, toEntity) {
    const rim = await page.locator(`[data-rim="${fromEntity}"]`).boundingBox();
    const target = await discCentre(page, toEntity);
    const start = { x: rim.x + (rim.width / 2), y: rim.y + (rim.height / 2) };
    await page.mouse.move(start.x, start.y);
    await page.mouse.down();
    // Through a point in between: the page treats a pointer that never travelled as a click.
    await page.mouse.move((start.x + target.x) / 2, (start.y + target.y) / 2, { steps: 8 });
    await page.mouse.move(target.x, target.y, { steps: 8 });
    await page.mouse.up();
}

async function editorOverAProject() {
    console.log("\nThe editor over a project (synqt design)");
    const project = await copyProject();
    const { proc, url } = await startEditor(project);
    const browser = await chromium.launch({ headless });
    const page = await browser.newPage();
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

        // Add a service, and connect it to the edge: the service owns the point, the edge
        // consumes it. Dropped the other way round the page would draw a different project.
        await page.locator(".palette__item", { hasText: "Service" }).click();
        await page.waitForSelector('[data-entity="service"]');
        await dragLink(page, "service", "web");
        await page.waitForFunction(
            () => document.querySelectorAll("[data-link]").length === 4);

        const inspector = page.locator("#inspector");
        await inspector.locator("input[type=text]").first().fill("audit");
        await inspector.locator("input[type=text]").nth(1).fill("Audit");

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
        check(diff.includes("synqt.yaml") && diff.includes("Audit.syn"),
              "the change set shows the configuration and the contract it would write");
        check(!fs.existsSync(path.join(project, "shared/Audit.syn")),
              "reviewing wrote nothing");

        await page.waitForSelector("#apply:not([disabled])");
        await page.locator("#apply").click();
        await waitForHint(page, "Applied");

        const config = await fsp.readFile(path.join(project, "synqt.yaml"), "utf8");
        check(/name:\s*service\b/.test(config), "synqt.yaml gained the service entity");
        check(/name:\s*audit\b/.test(config), "synqt.yaml gained the connect point");
        check(/contract:\s*Audit\b/.test(config) && /owner:\s*service\b/.test(config),
              "the point is owned by the entity it was dragged from");
        const contract = await fsp.readFile(path.join(project, "shared/Audit.syn"), "utf8");
        const drawn = /slot\s+logWinner\s*\(\s*string\s+winner\s*\)/.test(contract);
        check(drawn, drawn ? "shared/Audit.syn holds the slot that was drawn"
                           : `shared/Audit.syn holds something else:\n${contract.trim()}`);
        check(fs.existsSync(path.join(project, "service/Audit.qml")),
              "the owner got the Source file the point needs");
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
};

// The static host the documentation site is, near enough: it serves the files the hook
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
        await page.locator(".palette__item", { hasText: "Client" }).click();
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

await editorOverAProject();
await theCopyOnTheSite();

console.log("");
if (failures.length) {
    console.log(`designer: ${failures.length} check(s) failed`);
    process.exit(1);
}
console.log("designer: the editor writes what it draws, and the hosted copy stands alone");
