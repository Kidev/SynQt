// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's realistic HTTP column: Fastify and better-sqlite3, the stack a Node team would
// actually write these six routes in.
//
// Same six TechEmpower test types, same answers, same seed as http-bare.mjs and as
// benchmarks/edge/bench_edge.cpp: they share techempower.mjs precisely so a difference
// between the columns can only be the framework and the driver.
//
// Serves until killed; the loader is what measures.

import Database from "better-sqlite3";
import Fastify from "fastify";

import { parseArgs } from "./measure.mjs";
import {
    clampedQueryCount, FORTUNES, randomWorldId, renderFortunes, WORLD_ROWS,
} from "./techempower.mjs";

const args = parseArgs({ port: "8482" });
const port = Number.parseInt(args.port, 10);

const database = new Database(":memory:");
database.exec("CREATE TABLE world (id INTEGER PRIMARY KEY, randomNumber INTEGER NOT NULL)");
database.exec("CREATE TABLE fortune (id INTEGER PRIMARY KEY, message TEXT NOT NULL)");

const insertWorld = database.prepare("INSERT INTO world (id, randomNumber) VALUES (?, ?)");
const insertFortune = database.prepare("INSERT INTO fortune (id, message) VALUES (?, ?)");
database.transaction(() => {
    for (let id = 1; id <= WORLD_ROWS; id += 1) {
        insertWorld.run(id, 1 + Math.floor(Math.random() * WORLD_ROWS));
    }
    FORTUNES.forEach((message, index) => insertFortune.run(index + 1, message));
})();

const selectWorld = database.prepare("SELECT randomNumber FROM world WHERE id = ?");
const updateWorld = database.prepare("UPDATE world SET randomNumber = ? WHERE id = ?");
const selectFortunes = database.prepare("SELECT id, message FROM fortune");

const runUpdates = database.transaction((count) => {
    const rows = [];
    for (let index = 0; index < count; index += 1) {
        const id = randomWorldId();
        selectWorld.get(id);
        const randomNumber = 1 + Math.floor(Math.random() * WORLD_ROWS);
        updateWorld.run(randomNumber, id);
        rows.push({ id, randomNumber });
    }
    return rows;
});

// logger off: every column is measured without request logging, because one that logs and
// one that does not are not measuring the same thing.
const app = Fastify({ logger: false });

app.get("/plaintext", (_request, reply) => {
    reply.type("text/plain").send("Hello, World!");
});

app.get("/json", () => ({ message: "Hello, World!" }));

app.get("/db", () => {
    const id = randomWorldId();
    return { id, randomNumber: selectWorld.get(id).randomNumber };
});

app.get("/queries", (request) => {
    const count = clampedQueryCount(request.query.queries);
    const rows = [];
    for (let index = 0; index < count; index += 1) {
        const id = randomWorldId();
        rows.push({ id, randomNumber: selectWorld.get(id).randomNumber });
    }
    return rows;
});

app.get("/updates", (request) => runUpdates(clampedQueryCount(request.query.queries)));

app.get("/fortunes", (_request, reply) => {
    reply.type("text/html; charset=utf-8").send(renderFortunes(selectFortunes.all()));
});

await app.listen({ port, host: "127.0.0.1" });
console.log(`node-http-fastify listening on http://127.0.0.1:${port}`);
