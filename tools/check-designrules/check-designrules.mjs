// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The browser half of the design editor's rule check.
//
// tools/synqt/tests/test_designrules.py reads the same topologies.json and asserts that
// `synqt check` reaches the named verdict for every case. This asserts the page does too,
// which is the other half of the same claim: the editor paints a subset of the real rules,
// never a rule of its own. It needs node because rules.js is what the browser loads, and
// running the actual module is the only way to check the actual module.
//
// Nothing here is installed: it imports the shipped asset by path and reads a JSON file.

import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const here = path.dirname(fileURLToPath(import.meta.url));
const design = path.resolve(here, "..", "synqt", "synqt", "assets", "design");

const { findings } = await import(path.join(design, "rules.js"));
const cases = JSON.parse(fs.readFileSync(path.join(design, "topologies.json"), "utf8")).cases;

let failed = 0;

function fail(id, complaint) {
    console.error(`  ${id}: ${complaint}`);
    failed += 1;
}

for (const testCase of cases) {
    const found = findings(testCase.document);
    const match = found.find((finding) => finding.rule === testCase.rule);
    if (!match) {
        const others = found.map((finding) => finding.rule).join(", ") || "nothing";
        fail(testCase.id, `rules.js did not report '${testCase.rule}' (it reported ${others})`);
        continue;
    }
    if (match.level !== testCase.level) {
        fail(testCase.id, `'${testCase.rule}' is ${match.level} in rules.js and `
            + `${testCase.level} in topologies.json`);
    }
    if (!match.message) {
        fail(testCase.id, `'${testCase.rule}' reported nothing a reader could act on`);
    }
}

// A rule with no case is a rule nobody proved the command line agrees with. The Python
// suite makes the same assertion from its side; this one keeps the file honest for anyone
// who adds a rule and runs only node.
const declared = new Set([...fs.readFileSync(path.join(design, "rules.js"), "utf8")
    .matchAll(/rule:\s*"([a-z-]+)"/g)].map((found) => found[1]));
const covered = new Set(cases.map((testCase) => testCase.rule));
for (const rule of declared) {
    if (!covered.has(rule)) {
        fail(rule, "rules.js paints this rule and topologies.json has no case for it");
    }
}
for (const rule of covered) {
    if (!declared.has(rule)) {
        fail(rule, "topologies.json names this rule and rules.js does not paint it");
    }
}

if (failed > 0) {
    console.error(`\ndesign rules: ${failed} problem(s)`);
    process.exit(1);
}
console.log(`design rules: ${cases.length} topologies, ${declared.size} rules, all agreed`);
