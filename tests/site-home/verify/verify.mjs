// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The front page of the built documentation site, in a browser, at the widths it is laid out
// for. Nothing else looks at it: `mkdocs build --strict` proves the page builds, and says
// nothing about what it looks like once it is drawn.
//
//   node verify.mjs <site-dir>
//
// Checks:
//   1. every file the "What it looks like" panel opens fits in it, with no scrollbar either
//      way, at every width. The panel is one height for every file, measured from the tallest
//      (docs/javascripts/home-flow.js), and below the width where lines wrap that height
//      changes with the width;
//   2. in the narrow layout, where the tree sits over the file, the file starts at the top of
//      what is left rather than halfway down it;
//   3. "Why SynQt" draws a divider on the left of every card that has a card to its left, and
//      on no other, in three columns and in two;
//   4. the drawing lights the sign-in: hovering the edge shows the sign-in beside its line to
//      the client, and hovering the mark on the edge opens the card that says what it is.

import { chromium } from "playwright";
import { createReadStream, statSync } from "node:fs";
import { createServer } from "node:http";
import path from "node:path";

const site = path.resolve(process.argv[2] || "");
if (!process.argv[2] || !statSync(path.join(site, "index.html"), { throwIfNoEntry: false })) {
    console.log("usage: node verify.mjs <site-dir>  (a built site, with index.html in it)");
    process.exit(2);
}

const types = {
    ".html": "text/html", ".js": "text/javascript", ".css": "text/css",
    ".json": "application/json", ".svg": "image/svg+xml", ".png": "image/png",
    ".ico": "image/x-icon", ".woff2": "font/woff2", ".webp": "image/webp",
};

const server = createServer((request, response) => {
    let file = path.join(site, decodeURIComponent(new URL(request.url, "http://x").pathname));
    if (!file.startsWith(site)) {
        response.writeHead(403).end();
        return;
    }
    const found = statSync(file, { throwIfNoEntry: false });
    if (found && found.isDirectory()) {
        file = path.join(file, "index.html");
    }
    if (!statSync(file, { throwIfNoEntry: false })) {
        response.writeHead(404).end();
        return;
    }
    response.writeHead(200, { "Content-Type": types[path.extname(file)] || "application/octet-stream" });
    createReadStream(file).pipe(response);
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const origin = `http://127.0.0.1:${server.address().port}/`;

let failures = 0;
function check(ok, what) {
    console.log(`  ${ok ? "ok  " : "FAIL"}  ${what}`);
    if (!ok) {
        failures += 1;
    }
}

const browser = await chromium.launch({ args: ["--no-sandbox", "--disable-dev-shm-usage"] });

async function open(width, height = 1100) {
    const page = await browser.newPage({ viewport: { width, height } });
    await page.goto(origin, { waitUntil: "networkidle" });
    // The panel is sized once the drawing is in and the code font has arrived. A page whose
    // script never sizes it is measured anyway after a wait, so this reports its overruns
    // instead of timing out.
    await page.waitForFunction(() => {
        const view = document.querySelector(".synqt-explorer__view");
        return view && (view.style.height !== "" || performance.now() > 8000);
    }, null, { timeout: 20000 });
    await page.waitForTimeout(300);
    return page;
}

try {
    console.log("1. every file fits the panel");
    for (const width of [1400, 1100, 1000, 800, 700, 400, 340]) {
        const page = await open(width);
        const overruns = await page.evaluate(async () => {
            const found = [];
            const names = [...new Set([...document.querySelectorAll(".synqt-tree [data-file]")]
                .map((element) => element.dataset.file))];
            for (const name of names) {
                document.querySelector(`.synqt-tree [data-file="${name}"]`).focus();
                await new Promise((resolve) => requestAnimationFrame(resolve));
                for (const box of document.querySelectorAll(
                        ".synqt-file--current .highlight, .synqt-file--current pre > code")) {
                    const down = box.scrollHeight - box.clientHeight;
                    const across = box.scrollWidth - box.clientWidth;
                    if (down > 1 || across > 1) {
                        found.push(`${name} (${down} down, ${across} across)`);
                    }
                }
            }
            return found;
        });
        check(overruns.length === 0,
              `at ${width}px no file scrolls` + (overruns.length ? `: ${overruns.join(", ")}` : ""));
        await page.close();
    }

    console.log("2. the narrow layout starts the file at the top");
    for (const width of [800, 400]) {
        const page = await open(width);
        const gap = await page.evaluate(() => {
            document.querySelector('.synqt-tree [data-file="admin"]').focus();
            // The tree's rows, not its box: a grid row stretched to share the spare height
            // makes the box tall, and the gap under it small, with the file still far down.
            const tree = document.querySelector(".synqt-tree__list").getBoundingClientRect();
            const file = document.querySelector(".synqt-file--current").getBoundingClientRect();
            return file.top - tree.bottom;
        });
        check(gap >= 0 && gap < 40, `at ${width}px the file is right under the tree (${gap.toFixed(0)}px)`);
        await page.close();
    }

    console.log("3. the dividers between the reasons");
    for (const [width, columns] of [[1400, 3], [850, 2]]) {
        const page = await open(width);
        const cards = await page.evaluate(() => [...document.querySelectorAll(
            ".md-content--home .grid.cards > ul > li")].map((card) => ({
                left: card.getBoundingClientRect().left,
                border: parseFloat(getComputedStyle(card).borderLeftWidth),
                padding: parseFloat(getComputedStyle(card).paddingLeft),
            })));
        const firstColumn = Math.min(...cards.map((card) => card.left));
        const wrong = cards
            .map((card, index) => ({ ...card, index: index + 1,
                                     first: Math.abs(card.left - firstColumn) < 2 }))
            .filter((card) => card.first ? card.border > 0
                                         : !(card.border >= 1 && card.padding > 0));
        const seen = new Set(cards.map((card) => Math.round(card.left))).size;
        check(seen === columns, `at ${width}px the reasons sit in ${columns} columns (${seen})`);
        check(wrong.length === 0, `at ${width}px a divider on the left of every card but the `
              + "first in its row" + (wrong.length ? `, not on card ${wrong.map((c) => c.index)}` : ""));
        await page.close();
    }

    console.log("4. the drawing explains the sign-in");
    {
        const page = await open(1400);
        const stage = page.locator("#synqt-flow-stage");
        await stage.scrollIntoViewIfNeeded();
        const at = async (selector) => page.evaluate((wanted) => {
            const found = document.querySelector("#synqt-flow-stage").shadowRoot
                .querySelector(wanted);
            const box = found.getBoundingClientRect();
            return { x: box.x + (box.width / 2), y: box.y + (box.height / 2) };
        }, selector);
        const disc = await at(".node--edge .node__disc");
        await page.mouse.move(disc.x - 40, disc.y + 60);
        await page.mouse.move(disc.x, disc.y + 10, { steps: 5 });
        await page.waitForTimeout(300);
        const hinted = await page.evaluate(() => [...document.querySelector("#synqt-flow-stage")
            .shadowRoot.querySelectorAll(".link.is-signin .link__signin")]
            .filter((hint) => getComputedStyle(hint).display !== "none").length);
        check(hinted === 1, `hovering the edge shows the sign-in on its line to the client (${hinted})`);

        const mark = await at(".node--edge .signin");
        await page.mouse.move(mark.x - 30, mark.y - 30);
        await page.mouse.move(mark.x, mark.y, { steps: 5 });
        await page.waitForTimeout(300);
        const card = await page.evaluate(() => {
            const tip = document.querySelector("#synqt-flow-stage").shadowRoot.querySelector(".tip");
            return tip && !tip.hidden ? tip.textContent : "";
        });
        check(/Sign-in/.test(card) && /Session\.login\(\)/.test(card),
              "hovering the mark on the edge opens the sign-in card");
        await page.close();
    }
} catch (error) {
    console.log(`  FAIL  ${error.stack || error}`);
    failures += 1;
} finally {
    await browser.close();
    server.close();
}

console.log(failures === 0 ? "SITE HOME: PASS" : `SITE HOME: FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
