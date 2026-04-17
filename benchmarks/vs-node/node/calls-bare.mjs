// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's floor for the call comparison: `node:http`, a JSON body in and a JSON body out.
//
// Zero dependencies and no framework, on purpose. This is a control rather than a stack
// anybody deploys: it says how much of the Next.js Server Function number is Node answering
// a request at all, and how much is the machinery React puts around a function call. Without it "Next.js takes N microseconds" is a number
// with nothing to subtract from it.
//
// The two units of work are the same two ./nextjs/app/actions.js exports. `echo` has no
// database in it at all, so on that row the only difference between this column and the
// Next.js one is the framework; `lookup` reads the same seeded table but through a different
// driver, and that row is read with the caveat rather than without it.

import {createServer} from "node:http";
import {DatabaseSync} from "node:sqlite";

import {
    driveCalls, parseArgs, reportCall, residentBytes, summarizeCalls, writeResult,
} from "./measure.mjs";
import {WORLD_ROWS} from "./techempower.mjs";

const args = parseArgs({
    callers: "1,8,32,128",
    seconds: "5",
    work: "echo",
    out: "",
});

const sizes = String(args.callers).split(",")
    .map((value) => Number.parseInt(value.trim(), 10))
    .filter((value) => Number.isFinite(value) && value > 0);
const seconds = Math.max(1, Number.parseInt(args.seconds, 10));
const work = String(args.work);
if (work !== "echo" && work !== "lookup") {
    console.error(`--work is echo or lookup, not '${work}'`);
    process.exit(2);
}

console.log(`Node (node:http, JSON) call path: ${work}, ${seconds}s per size, `
            + `callers ${args.callers}`);

// The same table http-bare.mjs seeds, through Node's own SQLite rather than a package,
// which is what keeps this column dependency-free. It is also the one place this column and
// the Next.js one are not comparing like with like: `lookup` there goes through
// better-sqlite3, so the difference on that row is the driver as well as the framework. The
// `echo` row has no database in it at all and is the row that isolates the framework; the
// README says which to read for what.
const database = new DatabaseSync(":memory:");
database.exec("CREATE TABLE world (id INTEGER PRIMARY KEY, randomNumber INTEGER NOT NULL)");
const insertWorld = database.prepare("INSERT INTO world (id, randomNumber) VALUES (?, ?)");
for (let id = 1; id <= WORLD_ROWS; id += 1) {
    insertWorld.run(id, 1 + Math.floor(Math.random() * WORLD_ROWS));
}
const selectWorld = database.prepare("SELECT randomNumber FROM world WHERE id = ?");

function answer(name, argument) {
    if (name === "echo") {
        return argument;
    }
    const bounded = 1 + (Math.abs(Number(argument) || 0) % WORLD_ROWS);
    const row = selectWorld.get(bounded);
    return {id: bounded, randomNumber: row ? row.randomNumber : 0};
}

const server = createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
        let body;
        try {
            body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        } catch (error) {
            response.writeHead(400).end();
            return;
        }
        const payload = JSON.stringify(answer(request.url.slice(1), body[0]));
        response.writeHead(200, {
            "content-type": "application/json",
            "content-length": Buffer.byteLength(payload),
        });
        response.end(payload);
    });
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const origin = `http://127.0.0.1:${server.address().port}`;

async function call(argument) {
    const response = await fetch(`${origin}/${work}`, {
        method: "POST",
        headers: {"content-type": "application/json"},
        body: JSON.stringify([argument]),
    });
    if (!response.ok) {
        throw new Error(`the call answered ${response.status}`);
    }
    return response.json();
}

// Checked once, before the clock starts, on the same terms as every other column: a
// benchmark of a wrong endpoint is worse than no benchmark.
if (work === "echo") {
    const probe = await call("ping");
    if (probe !== "ping") {
        console.error(`echo returned ${JSON.stringify(probe)}, not "ping"`);
        process.exit(1);
    }
} else {
    const probe = await call(41);
    if (!probe || probe.id !== 42 || typeof probe.randomNumber !== "number") {
        console.error(`lookup returned ${JSON.stringify(probe)}`);
        process.exit(1);
    }
}

const baselineRss = residentBytes();
const sweep = [];

for (const callers of sizes) {
    const run = await driveCalls({
        callers,
        seconds,
        warmupCalls: 50,
        call: (index) => call(work === "echo" ? "ping" : index),
    });
    const entry = summarizeCalls({
        callers,
        ...run,
        rssPerCaller: callers > 0 ? (residentBytes() - baselineRss) / callers : 0,
        rssTotal: residentBytes(),
    });
    reportCall(entry);
    sweep.push(entry);
}

writeResult(args.out, {
    benchmark: "vs-node-calls",
    stack: "node-bare-call",
    path: "node:http, a JSON body in and a JSON body out",
    work,
    seconds,
    sweep,
});

server.close();
process.exit(0);
