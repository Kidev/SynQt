// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Parse every ```mermaid fence in the docs with mermaid itself, and fail on any that
// does not. mkdocs cannot catch this: superfences only relabels the fence as
// <pre class="mermaid">, and the theme parses it in the reader's browser, so a broken
// diagram builds green, deploys, and renders as "Syntax error in text" on the live page.
// That is exactly how five diagrams shipped with `node: "label" --> other`, which is not
// mermaid at all (the labelled edge is `node -->|"label"| other`).
//
// The mermaid range here must stay the one material/templates/assets loads at runtime
// (currently https://unpkg.com/mermaid@11/dist/mermaid.min.js). It floats on the major
// on purpose: pinning a patch here would check the docs against a parser the site does
// not use, which is a check that can pass while the page is broken.

import fs from "fs";
import path from "path";
import { JSDOM } from "jsdom";

const dom = new JSDOM("<!doctype html><html><body></body></html>");
global.window = dom.window;
global.document = dom.window.document;
Object.defineProperty(global, "navigator", { value: dom.window.navigator, configurable: true });
// mermaid.parse() reaches for DOMPurify on load; the real one needs a live DOM it never
// gets here, and sanitising is the browser's job at render time, not the parser's.
global.DOMPurify = { addHook() {}, sanitize: (html) => html, setConfig() {} };

const mermaid = (await import("mermaid")).default;
mermaid.initialize({ startOnLoad: false, securityLevel: "loose" });

function diagramsIn(file) {
    const found = [];
    const lines = fs.readFileSync(file, "utf8").split("\n");
    let inside = false;
    let start = 0;
    let buffer = [];
    for (let i = 0; i < lines.length; i++) {
        if (!inside && lines[i].trim() === "```mermaid") {
            inside = true;
            start = i + 2;  // the first line of the diagram, 1-based
            buffer = [];
        } else if (inside && lines[i].trim() === "```") {
            inside = false;
            found.push({ line: start, text: buffer.join("\n") });
        } else if (inside) {
            buffer.push(lines[i]);
        }
    }
    return found;
}

// Recursive, not one flat readdir: docs/ happens to be flat today, and a checker that
// quietly skips whichever subdirectory someone adds next is one that passes while the page
// is broken.
function markdownUnder(dir) {
    return fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name < b.name ? -1 : 1)
        .flatMap((entry) => {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                return markdownUnder(full);
            }
            return entry.name.endsWith(".md") ? [full] : [];
        });
}

const root = process.argv[2] || "docs";
let total = 0;
let failed = 0;
for (const file of markdownUnder(root)) {
    for (const diagram of diagramsIn(file)) {
        total++;
        try {
            await mermaid.parse(diagram.text);
        } catch (error) {
            failed++;
            const detail = String(error.message || error).split("\n")
                .map((line) => line.trimEnd()).filter((line) => line).join("\n    ");
            console.log(`${file}:${diagram.line}: mermaid rejects this diagram`);
            console.log(`    ${detail}\n`);
        }
    }
}
console.log(`${total} diagram(s) checked, ${failed} rejected.`);
process.exit(failed ? 1 : 0);
