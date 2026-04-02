// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

"use server";

// The Next.js column of the call comparison: Server Functions.
//
// This is the primitive that answers the same question SynQt's returning slot answers --
// the client asks the server to do something and waits for the value back -- so it is the
// one Next.js feature that lines up member-for-member with a connect point's slot. A Route
// Handler is the other way to do it, and the HTTP table already measures that; what is
// different here is everything React puts around the call: the action is addressed by a
// build-assigned id rather than by a URL, the arguments are decoded from the React flight
// format rather than parsed as JSON, and the return value is encoded back into it.
//
// Two functions, because one number would not separate the pipeline from the work:
//
//   echo    returns what it was handed. Nothing happens in the body, so what it measures is
//           the round trip and the framework around it, and nothing else.
//   lookup  reads one row from the same seeded table the six TechEmpower routes read, which
//           is the smallest honest unit of server-side work. The difference between the two
//           is what the call costs against what the work costs.
//
// The seeded database is ./store.js, which is what /db and /queries read, so this column and
// the HTTP table are answering out of one table and not two.

import {store} from "./store.js";

const WORLD_ROWS = 10000;

export async function echo(payload) {
    return payload;
}

export async function lookup(id) {
    const bounded = 1 + (Math.abs(Number(id) || 0) % WORLD_ROWS);
    const row = store().selectWorld.get(bounded);
    return {id: bounded, randomNumber: row ? row.randomNumber : 0};
}
