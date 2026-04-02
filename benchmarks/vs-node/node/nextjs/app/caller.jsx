// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

"use client";

// A client component that calls the two Server Functions, which is the only reason it is
// here: importing a server action from a client component is what makes `next build` treat
// it as a client-callable reference, assign it an id, and write that id into
// `.next/server/server-reference-manifest.json`. The harness reads the ids from there and
// makes the same POST React's runtime would.
//
// Without this file the actions still compile, but as server-only functions with no id and
// no route, and there would be nothing for the column to call.

import {echo, lookup} from "./actions.js";

export function Caller() {
    return (
        <button
            type="button"
            onClick={async () => {
                await echo("ping");
                await lookup(1);
            }}
        >
            call
        </button>
    );
}
