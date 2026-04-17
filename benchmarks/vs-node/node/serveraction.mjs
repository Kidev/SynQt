// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Calling a Next.js Server Function the way a browser calls one.
//
// A Server Function is not reached by a URL. `next build` assigns each one an id, writes it
// into `.next/server/server-reference-manifest.json`, and React's client runtime POSTs to
// the route of the page whose module graph contains it with that id in a `Next-Action`
// header. The arguments go up in React's flight encoding and the return value comes back in
// it. So there are exactly two honest ways to measure this path: drive a browser, or make
// the same request the browser's runtime makes. This is the second.
//
// The alternative (importing `actions.js` and calling the exported function) is
// not what this does. That measures the function body with Next.js removed
// from underneath it, which is the one thing this column exists to keep.
//
// The encoding is read out of the build rather than hard-coded, and what is not read out is
// verified on the first call (see `checkedAction`), so a Next release that changes the shape
// makes this say so instead of quietly measuring an error page.

import {readFileSync} from "node:fs";
import {join} from "node:path";

import {APP_DIR} from "./nextserver.mjs";

/// The ids `next build` assigned, by exported function name.
///
/// Empty for a build with no client-callable action in it, which is a real mistake and not a
/// state to carry on from: the caller checks and stops.
export function actionIds() {
    const manifest = JSON.parse(readFileSync(
        join(APP_DIR, ".next", "server", "server-reference-manifest.json"), "utf8"));
    const ids = {};
    for (const [id, entry] of Object.entries(manifest.node || {})) {
        if (entry.exportedName) {
            ids[entry.exportedName] = id;
        }
    }
    return ids;
}

/// The value a Server Function returned, out of the flight response.
///
/// The body is newline-separated `<row>:<json>` rows. The first row is the action's own
/// envelope and its `a` field points at the row holding the result, as `$@<row>`. Followed
/// rather than assumed to be row 1, because the envelope is what says where it is and a
/// page with more to send would put it elsewhere.
export function actionResult(body) {
    const rows = new Map();
    for (const line of body.split("\n")) {
        const cut = line.indexOf(":");
        if (cut <= 0) {
            continue;
        }
        rows.set(line.slice(0, cut), line.slice(cut + 1));
    }
    const envelope = rows.get("0");
    if (envelope === undefined) {
        throw new Error("the action response carried no envelope row");
    }
    const pointer = JSON.parse(envelope).a;
    if (typeof pointer !== "string" || !pointer.startsWith("$@")) {
        throw new Error(`the action envelope named no result (a=${JSON.stringify(pointer)})`);
    }
    const row = rows.get(pointer.slice(2));
    if (row === undefined) {
        throw new Error(`the action response has no row ${pointer.slice(2)}`);
    }
    return JSON.parse(row);
}

/// A function that calls one Server Function and resolves to what it returned.
///
/// `route` is the page the action is served on, which is the page whose graph holds it.
export function serverAction(origin, id, route = "/") {
    const url = `${origin}${route}`;
    const headers = {
        "Next-Action": id,
        // What React's runtime sends when every argument is JSON-serializable. With a File
        // or a FormData in the arguments it would send multipart instead; the arguments
        // here are a string and a number, so this is the encoding the real call uses.
        "Content-Type": "text/plain;charset=UTF-8",
    };
    return async (...args) => {
        const response = await fetch(url, {
            method: "POST",
            headers,
            body: JSON.stringify(args),
        });
        if (!response.ok) {
            throw new Error(`the action answered ${response.status}`);
        }
        return actionResult(await response.text());
    };
}

/// The same, with the first call checked against what it was supposed to return.
///
/// A benchmark of a wrong endpoint is worse than no benchmark, and this path has more ways
/// to be wrong than most: a stale build, an id that moved, a Next release that changed the
/// envelope. One assertion before the clock starts costs one call and rules all of that out.
export async function checkedAction(origin, id, route, expect) {
    const call = serverAction(origin, id, route);
    await expect(call);
    return call;
}
