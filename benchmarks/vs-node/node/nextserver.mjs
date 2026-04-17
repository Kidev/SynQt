// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Booting the Next.js column, shared by the two scripts that measure it.
//
// Programmatically and in this process, not `next start` in another one, and that is a
// measurement decision rather than a convenience. The live sweep stamps a frame on the
// publisher's clock and reads it back on the subscriber's, and every other column in this
// table does that inside one process on one monotonic clock; a Next server in a second
// process would turn the propagation figure into a difference between two clocks. The CPU
// and RSS figures have the same requirement: `process.cpuUsage()` is this process's.
//
// The cost of that decision is stated rather than hidden: this process holds the publisher,
// the subscribers and the server, exactly as the bare and Socket.IO columns do, so the
// three remain comparable to each other and none of them is a deployment topology.

import {createServer} from "node:http";
import {existsSync} from "node:fs";
import {dirname, join} from "node:path";
import {fileURLToPath} from "node:url";

export const APP_DIR = join(dirname(fileURLToPath(import.meta.url)), "nextjs");

// Where `next build` leaves its output. Its absence is the one failure worth naming: a
// production server started against no build serves 404 for every route, and a benchmark of
// a 404 is fast and meaningless.
function built() {
    return existsSync(join(APP_DIR, ".next", "BUILD_ID"));
}

/// Start the Next.js app on `port` (0 for one the OS picks) and answer the server and the
/// port it is on. Production mode: `dev: true` recompiles a route on first request and adds
/// a file watcher, neither of which is what anybody deploys.
export async function startNext(port) {
    if (!built()) {
        console.error(
            `no build in ${APP_DIR}/.next; run: (cd ${APP_DIR} && npx next build)`);
        process.exit(1);
    }
    const {default: next} = await import("next");
    const app = next({dev: false, dir: APP_DIR});
    await app.prepare();
    const handler = app.getRequestHandler();
    const server = createServer((request, response) => handler(request, response));
    // No timeout on a request: an SSE subscriber holds its response open for the whole
    // measured window, and Node's default would close it in the middle of one.
    server.requestTimeout = 0;
    server.headersTimeout = 0;
    server.keepAliveTimeout = 0;
    await new Promise((resolve) => server.listen(port, "127.0.0.1", resolve));
    return {app, server, port: server.address().port};
}

/// The live route's subscriber set, reached through the symbol the route publishes it on.
/// Next gives each route its own server bundle, so importing the route from here would
/// give a second module with a second empty Set in it; see nextjs/app/live/route.js.
export function liveArena() {
    return globalThis[Symbol.for("synqt.vs-node.nextjs.live")] || null;
}
