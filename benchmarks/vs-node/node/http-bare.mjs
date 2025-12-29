// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's bare HTTP column: node:http and node:sqlite, no dependencies.
//
// The six TechEmpower test types, answering exactly what benchmarks/edge/bench_edge.cpp
// answers for SynQt, so the two are comparable route by route. Driven by the same
// dependency-free keep-alive loader benchmarks/edge already uses, which is what keeps the
// generator from being a variable between the columns.
//
// Serves until killed; the loader is what measures.

import { createServer } from "node:http";
import { DatabaseSync } from "node:sqlite";

import { parseArgs } from "./measure.mjs";
import {
    clampedQueryCount, FORTUNES, randomWorldId, renderFortunes, WORLD_ROWS,
} from "./techempower.mjs";

const args = parseArgs({ port: "8481" });
const port = Number.parseInt(args.port, 10);

const database = new DatabaseSync(":memory:");
database.exec("CREATE TABLE world (id INTEGER PRIMARY KEY, randomNumber INTEGER NOT NULL)");
database.exec("CREATE TABLE fortune (id INTEGER PRIMARY KEY, message TEXT NOT NULL)");

const insertWorld = database.prepare("INSERT INTO world (id, randomNumber) VALUES (?, ?)");
const insertFortune = database.prepare("INSERT INTO fortune (id, message) VALUES (?, ?)");
database.exec("BEGIN");
for (let id = 1; id <= WORLD_ROWS; id += 1) {
    insertWorld.run(id, 1 + Math.floor(Math.random() * WORLD_ROWS));
}
FORTUNES.forEach((message, index) => insertFortune.run(index + 1, message));
database.exec("COMMIT");

const selectWorld = database.prepare("SELECT randomNumber FROM world WHERE id = ?");
const updateWorld = database.prepare("UPDATE world SET randomNumber = ? WHERE id = ?");
const selectFortunes = database.prepare("SELECT id, message FROM fortune");

function sendJson(response, value) {
    const body = JSON.stringify(value);
    response.writeHead(200, {
        "Content-Type": "application/json",
        "Content-Length": Buffer.byteLength(body),
    });
    response.end(body);
}

const server = createServer((request, response) => {
    const url = new URL(request.url, "http://127.0.0.1");
    const count = () => clampedQueryCount(url.searchParams.get("queries"));

    switch (url.pathname) {
    case "/plaintext": {
        const body = "Hello, World!";
        response.writeHead(200, {
            "Content-Type": "text/plain",
            "Content-Length": Buffer.byteLength(body),
        });
        response.end(body);
        return;
    }
    case "/json":
        sendJson(response, { message: "Hello, World!" });
        return;
    case "/db": {
        const id = randomWorldId();
        const row = selectWorld.get(id);
        sendJson(response, { id, randomNumber: row.randomNumber });
        return;
    }
    case "/queries": {
        const rows = [];
        for (let index = 0; index < count(); index += 1) {
            const id = randomWorldId();
            rows.push({ id, randomNumber: selectWorld.get(id).randomNumber });
        }
        sendJson(response, rows);
        return;
    }
    case "/updates": {
        const rows = [];
        database.exec("BEGIN");
        for (let index = 0; index < count(); index += 1) {
            const id = randomWorldId();
            selectWorld.get(id);
            const randomNumber = 1 + Math.floor(Math.random() * WORLD_ROWS);
            updateWorld.run(randomNumber, id);
            rows.push({ id, randomNumber });
        }
        database.exec("COMMIT");
        sendJson(response, rows);
        return;
    }
    case "/fortunes": {
        const body = renderFortunes(selectFortunes.all());
        response.writeHead(200, {
            "Content-Type": "text/html; charset=utf-8",
            "Content-Length": Buffer.byteLength(body),
        });
        response.end(body);
        return;
    }
    default:
        response.writeHead(404);
        response.end();
    }
});

server.keepAliveTimeout = 60000;
server.listen(port, "127.0.0.1", () => {
    console.log(`node-http-bare listening on http://127.0.0.1:${port}`);
});
