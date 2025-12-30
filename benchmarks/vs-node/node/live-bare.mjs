// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's bare column of the live-path comparison: node:http plus a hand-rolled RFC 6455
// server (wsserver.mjs) and the global WebSocket client Node 22 ships. Zero dependencies.
//
// This is the fastest honest Node, which is exactly why it is here: it is the column SynQt
// cannot be accused of sandbagging. It is also a stack almost nobody ships, which is why
// live-socketio.mjs sits beside it.
//
// Publisher and subscribers share one process, as they do on the SynQt side, so both
// columns time an interval on one monotonic clock rather than across two.

import {
    cpuMilliseconds, makeFrame, nowMicros, parseArgs, readStamp, report, residentBytes,
    sleep, summarize, writeResult,
} from "./measure.mjs";
import { startBroadcastServer } from "./wsserver.mjs";

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
    `Node (bare) live path: ${seconds}s at ${saturate ? "saturation" : `${hz} Hz`}, ${payloadBytes} byte payload, ` +
    `subscribers ${args.subscribers}`);

const payload = Buffer.alloc(payloadBytes, 0x78);
const baselineRss = residentBytes();
const sweep = [];

for (const subscriberCount of sizes) {
    const server = await startBroadcastServer();
    const propagation = [];
    let delivered = 0;
    let deliveredThisFrame = 0;
    let measuring = false;

    const sockets = [];
    for (let i = 0; i < subscriberCount; i += 1) {
        const socket = new WebSocket(`ws://127.0.0.1:${server.port}`);
        socket.binaryType = "arraybuffer";
        socket.addEventListener("message", (event) => {
            if (!measuring) {
                return;
            }
            const frame = Buffer.from(event.data);
            propagation.push((nowMicros() - readStamp(frame)) / 1000);
            delivered += 1;
            deliveredThisFrame += 1;
        });
        sockets.push(socket);
    }

    const deadline = Date.now() + 30000;
    while (server.clientCount() < subscriberCount && Date.now() < deadline) {
        await sleep(10);
    }
    if (server.clientCount() < subscriberCount) {
        console.error(`subscribers did not come up at N=${subscriberCount}`);
        process.exit(1);
    }
    const connectedRss = residentBytes();

    // Warm up before measuring, for the same reason the SynQt side does: the first frames
    // pay for lazily created buffers on both ends and would otherwise be the whole tail.
    const warmupTicks = Math.min(hz, 30);
    for (let tick = 0; tick < warmupTicks; tick += 1) {
        server.broadcast(makeFrame(nowMicros(), payload));
        await sleep(1000 / hz);
    }

    measuring = true;
    let ticks = seconds * hz;
    const cpuBefore = cpuMilliseconds();
    const startedAt = Date.now();
    if (saturate) {
        // Closed loop: publish, wait for the whole fleet to have it, publish again. Open
        // -looping at "maximum rate" would measure the send buffer rather than the
        // capacity, and the SynQt side cannot open-loop at all (QtRO coalesces outbound
        // property changes), so this is the mode both columns share.
        ticks = 0;
        while (Date.now() - startedAt < seconds * 1000) {
            deliveredThisFrame = 0;
            server.broadcast(makeFrame(nowMicros(), payload));
            ticks += 1;
            const frameDeadline = Date.now() + 5000;
            while (deliveredThisFrame < subscriberCount && Date.now() < frameDeadline) {
                // setImmediate, not sleep(0). A zero-millisecond setTimeout still goes
                // through the timer phase and does not fire faster than about a
                // millisecond, so waiting on it caps this loop at ~800 frames a second
                // whatever Node can actually do: the first version of this measured 31k
                // msg/s and reported Node as scaling 1.14x over four processes, which was
                // a fact about the wait and not about Node. setImmediate runs in the check
                // phase, right after the I/O the deliveries arrive on.
                await new Promise((resolve) => setImmediate(resolve));
            }
            if (deliveredThisFrame < subscriberCount) {
                console.error(`a frame never reached every subscriber at N=${subscriberCount}`);
                process.exit(1);
            }
        }
    } else {
        for (let tick = 0; tick < ticks; tick += 1) {
            server.broadcast(makeFrame(nowMicros(), payload));
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

    for (const socket of sockets) {
        socket.close();
    }
    server.close();
    await sleep(100);
}

writeResult(args.out, {
    stack: "node-bare",
    path: "node:http + hand-rolled RFC 6455",
    hz: saturate ? 0 : hz,
    saturated: saturate,
    seconds,
    payload_bytes: payloadBytes,
    sweep,
});
