// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Ask TypeScript what type an expression in a project's QML has.
//
// The Python side cuts the JavaScript out of each QML file and asks by where an
// expression was written, not by where it landed here, so this reads a request of
// {"files": {"<qml path>": "<synthesized module>"}, "queries": [{expression, file, line}]}
// on stdin and writes {"answers": [{"type": "..."}]} on stdout, one answer per query in
// the order they were asked.
//
// The type names are a .syn contract's, and "var" is the answer to everything TypeScript
// could not pin down. Nothing here guesses: a wrong type in a contract outlives the guess
// that put it there.

import { createRequire } from "node:module";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));

// The standard ways JavaScript makes a whole number. TypeScript calls what they return a
// number like any other, and every one of them is specified to return an integral value,
// so this is reading the language rather than guessing at the code.
const WHOLE_CALLS = ["parseInt", "Math.floor", "Math.ceil", "Math.round", "Math.trunc"];

// ts-morph is looked for beside this script first and in the working directory second, so
// that either `npm install ts-morph` in the project or one installed alongside SynQt
// answers. Node resolves a bare specifier from the importing file only, which is why this
// asks twice rather than importing once.
function loadTsMorph() {
    const roots = [HERE, process.cwd()];
    let last = null;
    for (const root of roots) {
        try {
            return createRequire(path.join(root, "noop.cjs"))("ts-morph");
        } catch (error) {
            last = error;
        }
    }
    throw last || new Error("ts-morph is not installed");
}

// A .syn contract has four types and an escape hatch. A TypeScript number is a real
// unless every literal behind it was whole, which is the difference between a count and a
// coordinate and the only place this looks past the type at the values.
function synType(node, type) {
    if (type.isAny() || type.isUnknown() || type.isNever()) {
        return "var";
    }
    if (type.isString() || type.isStringLiteral()) {
        return "string";
    }
    if (type.isBoolean() || type.isBooleanLiteral()) {
        return "bool";
    }
    if (type.isNumber() || type.isNumberLiteral()) {
        return wholeNumbers(node, type) ? "int" : "real";
    }
    return "var";
}

// The literals behind a number: the expression's own type when it is one, and otherwise
// the initializer of whatever it was declared from. An expression whose literals cannot
// be found at all is a real, because a coordinate is the commoner thing to have lost.
function wholeNumbers(node, type) {
    if (node.getKindName() === "CallExpression"
            && WHOLE_CALLS.includes(node.getExpression().getText())) {
        return true;
    }
    const seen = [];
    if (type.isNumberLiteral()) {
        seen.push(type.getLiteralValue());
    } else {
        const declaration = node.getSymbol()?.getValueDeclaration();
        const initializer = declaration?.getInitializer?.();
        const declared = initializer?.getType();
        if (declared?.isNumberLiteral()) {
            seen.push(declared.getLiteralValue());
        }
    }
    return seen.length > 0 && seen.every((value) => Number.isInteger(value));
}

// Where in the synthesized module a QML line landed: the region that covers it, then that
// many line breaks into the region. Nothing wrapped around a region adds a line to it, so
// its Nth line is the QML's Nth line and this arrives at the line that was asked about.
// The region it ends at is returned too, so a search for the expression cannot wander into
// the next one and answer about something else entirely.
function region(text, query) {
    const marker = new RegExp(`^// ${escaped(query.file)}:(\\d+)-(\\d+)$`, "gm");
    for (let found = marker.exec(text); found; found = marker.exec(text)) {
        const first = Number(found[1]);
        if (query.line < first || query.line > Number(found[2])) {
            continue;
        }
        let at = found.index + found[0].length + 1;
        for (let remaining = query.line - first; remaining > 0; remaining -= 1) {
            at = text.indexOf("\n", at) + 1;
            if (at <= 0) {
                return null;
            }
        }
        const next = marker.exec(text);
        return { from: at, until: next ? next.index : text.length };
    }
    return null;
}

function escaped(text) {
    return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// Find the expression the query names: the line it was written on, then the expression
// itself on it. The synthesized module keeps every region character for character, so the
// text a query carries is the text that is in there.
function locate(sourceFile, query) {
    const text = sourceFile.getFullText();
    const where = region(text, query);
    if (!where) {
        return null;
    }
    // Every occurrence, not only the first: "auction.highBid" is also the opening of
    // "auction.highBidder", and matching text is not the same as being that expression.
    for (let start = text.indexOf(query.expression, where.from);
         start >= 0 && start < where.until;
         start = text.indexOf(query.expression, start + 1)) {
        let node = sourceFile.getDescendantAtPos(start);
        let widest = null;
        while (node && node.getStart() === start) {
            if (node.getText() === query.expression) {
                widest = node;
            }
            node = node.getParent();
        }
        if (widest) {
            return widest;
        }
    }
    return null;
}

function answer(project, query) {
    const sourceFile = project.getSourceFile(`${query.file}.js`);
    if (!sourceFile) {
        return { type: "var" };
    }
    const node = locate(sourceFile, query);
    if (!node) {
        return { type: "var" };
    }
    try {
        return { type: synType(node, node.getType()) };
    } catch {
        // A checker that cannot type a node has said what it knows, which is nothing.
        return { type: "var" };
    }
}

function read() {
    return new Promise((resolve, reject) => {
        let body = "";
        process.stdin.setEncoding("utf8");
        process.stdin.on("data", (chunk) => { body += chunk; });
        process.stdin.on("end", () => resolve(body));
        process.stdin.on("error", reject);
    });
}

async function main() {
    const tsMorph = loadTsMorph();
    if (process.argv.includes("--probe")) {
        process.stdout.write(JSON.stringify({ answers: [] }));
        return;
    }

    const request = JSON.parse((await read()) || "{}");
    const project = new tsMorph.Project({
        useInMemoryFileSystem: true,
        compilerOptions: {
            allowJs: true,
            checkJs: false,
            noEmit: true,
            strict: false,
            target: tsMorph.ts.ScriptTarget.ESNext,
            module: tsMorph.ts.ModuleKind.ESNext,
        },
    });
    for (const [name, source] of Object.entries(request.files || {})) {
        // The .js suffix is what makes TypeScript read it as JavaScript; the name in front
        // of it stays the QML path, so a query names one file rather than two.
        project.createSourceFile(`${name}.js`, source, { overwrite: true });
    }
    const answers = (request.queries || []).map((query) => answer(project, query));
    process.stdout.write(JSON.stringify({ answers }));
}

main().catch((error) => {
    process.stdout.write(JSON.stringify({ error: String(error && error.message || error) }));
    process.exitCode = 1;
});
