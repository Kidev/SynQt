// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The live path, as Next.js actually offers one.
//
// Next.js has no WebSocket server. What it has for "N subscribers see every change" is a
// Route Handler returning a ReadableStream as `text/event-stream`, which is the thing a
// Next team builds this out of before they reach for a second process. So that is the
// column: the framework is genuinely in the data path, holding the connections and writing
// the frames, and what is measured is what that costs.
//
// The alternative (a custom server with `ws` bolted onto it) is on purpose not a
// column here. It is `node-bare` with a Next.js process sitting beside it: the frames never
// touch Next, and printing that as "Next.js" would be measuring one stack and labelling it
// another. See the README.
//
// The frame is the same eight bytes of microsecond stamp and the same payload as every other
// column, base64 into one `data:` line, because SSE is a text protocol. That costs a third
// more bytes on the wire and one encode per publish, done once here, outside the loop, so
// that adding a subscriber does not add an encode. It is the honest cost of this design
// rather than an artefact of the harness: a Next.js deployment doing this pays it too.

const HELD = Symbol.for("synqt.vs-node.nextjs.live");

// On `globalThis`, so the publisher can reach the subscribers this route is holding. Two
// reasons, and the second is the one that forces it: the App Router gives each route its own
// server bundle, so a module-level Set here is not the Set anything outside this file sees.
export function arena() {
    if (!globalThis[HELD]) {
        globalThis[HELD] = {
            open: new Set(),
            // What a publisher calls. Named on the object rather than exported, because the
            // harness reaches this through the symbol and never imports the route.
            publish(frame) {
                const line = `data: ${frame.toString("base64")}\n\n`;
                const bytes = new TextEncoder().encode(line);
                for (const controller of globalThis[HELD].open) {
                    controller.enqueue(bytes);
                }
                return globalThis[HELD].open.size;
            },
        };
    }
    return globalThis[HELD];
}

export const dynamic = "force-dynamic";

export function GET(request) {
    const held = arena();
    let mine = null;
    const body = new ReadableStream({
        start(controller) {
            mine = controller;
            held.open.add(controller);
        },
        cancel() {
            held.open.delete(mine);
        },
    });
    // The client going away has to take its controller with it, or the publisher writes into
    // a stream nobody is reading and the subscriber count the sweep believes is not the one
    // the frames are going to.
    request.signal.addEventListener("abort", () => {
        held.open.delete(mine);
    });
    return new Response(body, {
        headers: {
            "content-type": "text/event-stream",
            "cache-control": "no-store, no-transform",
            connection: "keep-alive",
        },
    });
}
