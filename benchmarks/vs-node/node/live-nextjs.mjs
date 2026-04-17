// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's framework column of the live-path comparison: Next.js, doing the only live path
// Next.js has.
//
// Next.js ships no WebSocket server. The way a Next team serves "N subscribers see every
// change" without leaving the framework is a Route Handler returning a ReadableStream as
// `text/event-stream`, and that is what nextjs/app/live/route.js is. The framework holds the
// connections and writes the frames, so this measures Next.js rather than measuring
// something beside it.
//
// What it is not: a custom server with `ws` attached. That is `node-bare` with a Next.js
// process next to it (the frames never touch Next), and printing it under this heading
// would be labelling one stack with another's name. See README.md.
//
// Two things differ from the other columns, both stated rather than corrected for, because
// both are what this design actually costs a deployment:
//
//  * SSE is text, so the same eight-byte stamp and the same payload travel base64 in one
//    `data:` line: a third more bytes on the wire, and an encode per publish. Per publish
//    and not per subscriber (the route encodes once and enqueues the same bytes to every
//    open stream), so this is a fixed cost rather than the marginal one.
//  * SSE is one direction. There is nothing to compare on the way back, and this table
//    never measured that anyway: the workload is one publisher, N subscribers.
//
// Everything else is held constant with live-bare.mjs and live-socketio.mjs: the same clock,
// the same warm-up, the same measured window, the same drain, the same statistics, and
// publisher, subscribers and server in one process.

import {
    cpuMilliseconds, makeFrame, nowMicros, parseArgs, readStamp, report, residentBytes,
    sleep, summarize, writeResult,
} from "./measure.mjs";
import { liveArena, startNext } from "./nextserver.mjs";

const args = parseArgs({
    subscribers: "10,50,100,250",
    seconds: "5",
    hz: "30",
    payload: "256",
    saturate: false,
    out: "",
});

const sizes = String(args.subscribers).split(",")
    .map((value) => Number.parseInt(value.trim(), 10))
    .filter((value) => Number.isFinite(value) && value > 0);
const seconds = Math.max(1, Number.parseInt(args.seconds, 10));
const hz = Math.max(1, Number.parseInt(args.hz, 10));
const payloadBytes = Math.max(0, Number.parseInt(args.payload, 10));
const saturate = args.saturate === true || args.saturate === "true";

console.log(
    `Node (Next.js, server-sent events) live path: ${seconds}s at ${
        saturate ? "saturation" : `${hz} Hz`}, ${payloadBytes} byte payload, ` +
    `subscribers ${args.subscribers}`);

// One server for the whole sweep, unlike the other two columns, and for a reason worth
// writing down: `app.prepare()` is seconds of work, and standing a Next.js server up four
// times would put four cold starts inside a run whose first number is a warm-up. The
// subscribers are what the sweep opens and closes, which is what the sweep is about.
const {app, server} = await startNext(0);
const port = server.address().port;

const payload = Buffer.alloc(payloadBytes, 0x78);
const baselineRss = residentBytes();
const sweep = [];

// One subscriber: a streaming GET, read frame by frame. `fetch` and a reader rather than
// `EventSource`, so what parses the stream is this file for every column that needs a parser
// the bare column hand-rolls its RFC 6455 reader for the same reason.
function subscribe(url, onFrame) {
    const stop = new AbortController();
    const started = fetch(url, {signal: stop.signal, headers: {accept: "text/event-stream"}})
        .then(async (response) => {
            if (!response.ok) {
                throw new Error(`the live route answered ${response.status}`);
            }
            const reader = response.body.getReader();
            let held = "";
            for (;;) {
                const {value, done} = await reader.read();
                if (done) {
                    return;
                }
                held += Buffer.from(value).toString("latin1");
                // An SSE event ends at a blank line. A chunk can hold several and can end
                // mid-event, so what is kept is whatever follows the last complete one.
                let cut = held.indexOf("\n\n");
                while (cut !== -1) {
                    const event = held.slice(0, cut);
                    held = held.slice(cut + 2);
                    if (event.startsWith("data: ")) {
                        onFrame(Buffer.from(event.slice(6), "base64"));
                    }
                    cut = held.indexOf("\n\n");
                }
            }
        })
        .catch((error) => {
            if (error.name !== "AbortError") {
                throw error;
            }
        });
    return {stop, started};
}

for (const subscriberCount of sizes) {
    const propagation = [];
    let delivered = 0;
    let deliveredThisFrame = 0;
    let measuring = false;

    const open = [];
    for (let i = 0; i < subscriberCount; i += 1) {
        open.push(subscribe(`http://127.0.0.1:${port}/live`, (frame) => {
            if (!measuring) {
                return;
            }
            propagation.push((nowMicros() - readStamp(frame)) / 1000);
            delivered += 1;
            deliveredThisFrame += 1;
        }));
    }

    // The route registers each stream as it starts, so the count the publisher will write to
    // is the thing to wait on: a subscriber whose request has been sent but whose handler has
    // not run yet is a subscriber the first frames would miss.
    const deadline = Date.now() + 30000;
    while ((liveArena()?.open.size || 0) < subscriberCount && Date.now() < deadline) {
        await sleep(10);
    }
    const arena = liveArena();
    if (!arena || arena.open.size < subscriberCount) {
        console.error(`subscribers did not come up at N=${subscriberCount}`);
        process.exit(1);
    }
    const connectedRss = residentBytes();

    // Warm up before measuring, for the same reason every other column does: the first
    // frames pay for lazily created buffers on both ends and would otherwise be the tail.
    const warmupTicks = Math.min(hz, 30);
    for (let tick = 0; tick < warmupTicks; tick += 1) {
        arena.publish(makeFrame(nowMicros(), payload));
        await sleep(1000 / hz);
    }

    measuring = true;
    let ticks = seconds * hz;
    const cpuBefore = cpuMilliseconds();
    const startedAt = Date.now();
    if (saturate) {
        // The closed loop the other columns use: publish, wait for the whole fleet to have
        // it, publish again. Open-looping would measure the send buffer rather than the
        // capacity, and the SynQt column cannot open-loop at all.
        ticks = 0;
        while (Date.now() - startedAt < seconds * 1000) {
            deliveredThisFrame = 0;
            arena.publish(makeFrame(nowMicros(), payload));
            ticks += 1;
            const frameDeadline = Date.now() + 5000;
            while (deliveredThisFrame < subscriberCount && Date.now() < frameDeadline) {
                // setImmediate rather than a zero-millisecond timer, for the reason
                // live-bare.mjs spells out: a timer does not fire faster than about a
                // millisecond and would cap this loop far below what the stack can do.
                await new Promise((resolve) => setImmediate(resolve));
            }
            if (deliveredThisFrame < subscriberCount) {
                console.error(`a frame never reached every subscriber at N=${subscriberCount}`);
                process.exit(1);
            }
        }
    } else {
        for (let tick = 0; tick < ticks; tick += 1) {
            arena.publish(makeFrame(nowMicros(), payload));
            const due = startedAt + Math.round(((tick + 1) * 1000) / hz);
            const wait = due - Date.now();
            if (wait > 0) {
                await sleep(wait);
            }
        }
    }
    // Let what is in flight land, or the tail of every run reads as loss that is really the
    // harness stopping first.
    if (!saturate) {
        await sleep(500);
    }
    const elapsedSeconds = (Date.now() - startedAt) / 1000;
    const cpuMs = cpuMilliseconds() - cpuBefore;
    measuring = false;

    const entry = summarize({
        subscribers: subscriberCount,
        propagation,
        delivered,
        expected: ticks * subscriberCount,
        elapsedSeconds,
        cpuMs,
        rssPerConnection: subscriberCount > 0
            ? (connectedRss - baselineRss) / subscriberCount
            : 0,
        rssTotal: connectedRss,
    });
    report(entry);
    sweep.push(entry);

    for (const one of open) {
        one.stop.abort();
    }
    await Promise.all(open.map((one) => one.started));
    // The abort reaches the route asynchronously, and the next size waits on the count this
    // one leaves behind.
    const emptyBy = Date.now() + 5000;
    while (arena.open.size > 0 && Date.now() < emptyBy) {
        await sleep(10);
    }
    await sleep(100);
}

writeResult(args.out, {
    stack: "node-nextjs",
    path: "Next.js 16 App Router, Route Handler streaming server-sent events",
    hz: saturate ? 0 : hz,
    saturated: saturate,
    seconds,
    payload_bytes: payloadBytes,
    sweep,
});

server.close();
await app.close?.();
// Next keeps handles alive past close(); the measurement is done, so end deliberately
// rather than let the process hang looking finished. Socket.IO's column ends the same way.
process.exit(0);
