// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The seeded database the six routes read, and the one place it is opened.
//
// On `globalThis` and not in a module variable, which is the thing about Next.js this file
// exists to get right: the App Router compiles each route into its own server bundle, so a
// module imported by six routes is instantiated more than once and six copies of this would
// mean six ten-thousand-row tables seeded independently. It is also the documented Next
// pattern for a database handle, for exactly this reason.
//
// The schema, the row count, the fortunes and the seeding are ../../../techempower.mjs and
// bench_edge.cpp's, not this file's: every column answers out of the same description.

import Database from "better-sqlite3";

import {FORTUNES, WORLD_ROWS} from "../../techempower.mjs";

const HELD = Symbol.for("synqt.vs-node.nextjs.store");

function opened() {
    const database = new Database(":memory:");
    database.exec("CREATE TABLE world (id INTEGER PRIMARY KEY, "
                  + "randomNumber INTEGER NOT NULL)");
    database.exec("CREATE TABLE fortune (id INTEGER PRIMARY KEY, message TEXT NOT NULL)");

    const insertWorld = database.prepare(
        "INSERT INTO world (id, randomNumber) VALUES (?, ?)");
    const insertFortune = database.prepare(
        "INSERT INTO fortune (id, message) VALUES (?, ?)");
    database.transaction(() => {
        for (let id = 1; id <= WORLD_ROWS; id += 1) {
            insertWorld.run(id, 1 + Math.floor(Math.random() * WORLD_ROWS));
        }
        FORTUNES.forEach((message, index) => insertFortune.run(index + 1, message));
    })();

    const selectWorld = database.prepare("SELECT randomNumber FROM world WHERE id = ?");
    const updateWorld = database.prepare("UPDATE world SET randomNumber = ? WHERE id = ?");
    const selectFortunes = database.prepare("SELECT id, message FROM fortune");
    // One transaction for the whole batch, the same as the Fastify column: a batch of
    // updates run one statement at a time is a different test.
    const runUpdates = database.transaction((count) => {
        const rows = [];
        for (let index = 0; index < count; index += 1) {
            const id = 1 + Math.floor(Math.random() * WORLD_ROWS);
            selectWorld.get(id);
            const randomNumber = 1 + Math.floor(Math.random() * WORLD_ROWS);
            updateWorld.run(randomNumber, id);
            rows.push({id, randomNumber});
        }
        return rows;
    });

    return {selectWorld, selectFortunes, runUpdates};
}

export function store() {
    if (!globalThis[HELD]) {
        globalThis[HELD] = opened();
    }
    return globalThis[HELD];
}
