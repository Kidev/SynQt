// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's realistic column of the live-path comparison: Socket.IO, which is what a Node team
// building this actually reaches for.
//
// It measures the same interval, on the same clock, over the same frame layout as
// live-bare.mjs, so the only difference between the two columns is the stack. That is the
// point of having both: the bare column is the floor SynQt has to beat and nobody ships it,
// this one is what people deploy and it flatters SynQt, and either alone is arguable.
//
// Socket.IO is given its best case here. The transport is pinned to "websocket" so no run
// starts on HTTP long-polling and upgrades mid-measurement, `perMessageDeflate` stays off
// (compressing a random payload spends CPU to no end, and the SynQt side does not compress
// either), and the payload travels as a Buffer so it is a binary frame rather than base64.

import { createServer } from "node:http";

import { Server } from "socket.io";
import { io as connect } from "socket.io-client";

import {
    cpuMilliseconds, makeFrame, nowMicros, parseArgs, readStamp, report, residentBytes,
    sleep, summarize, writeResult,
} from "./measure.mjs";

const args = parseArgs({
    subscribers: "10,50,100,250",
    seconds: "5",
    hz: "30",
    payload: "256",
    out: "",
});

const sizes = String(args.subscribers).split(",")
    .map((value) => Number.parseInt(value.trim(), 10))
    .filter((value) => Number.isFinite(value) && value > 0);
const seconds = Math.max(1, Number.parseInt(args.seconds, 10));
const hz = Math.max(1, Number.parseInt(args.hz, 10));
const payloadBytes = Math.max(0, Number.parseInt(args.payload, 10));

console.log(
    `Node (Socket.IO) live path: ${seconds}s at ${hz} Hz, ${payloadBytes} byte payload, ` +
    `subscribers ${args.subscribers}`);

const payload = Buffer.alloc(payloadBytes, 0x78);
const baselineRss = residentBytes();
const sweep = [];

for (const subscriberCount of sizes) {
    const http = createServer();
    const server = new Server(http, {
        transports: ["websocket"],
        perMessageDeflate: false,
        maxHttpBufferSize: 1e7,
    });
    let connected = 0;
    server.on("connection", () => {
        connected += 1;
    });
    await new Promise((resolve) => http.listen(0, "127.0.0.1", resolve));
    const port = http.address().port;

    const propagation = [];
    let delivered = 0;
    let measuring = false;

    const clients = [];
    for (let i = 0; i < subscriberCount; i += 1) {
        const client = connect(`ws://127.0.0.1:${port}`, {
            transports: ["websocket"],
            forceNew: true,
            reconnection: false,
        });
        client.on("frame", (frame) => {
            if (!measuring) {
                return;
            }
            propagation.push((nowMicros() - readStamp(Buffer.from(frame))) / 1000);
            delivered += 1;
        });
        clients.push(client);
    }

    const deadline = Date.now() + 30000;
    while (connected < subscriberCount && Date.now() < deadline) {
        await sleep(10);
    }
    if (connected < subscriberCount) {
        console.error(`subscribers did not come up at N=${subscriberCount}`);
        process.exit(1);
    }
    const connectedRss = residentBytes();

    const warmupTicks = Math.min(hz, 30);
    for (let tick = 0; tick < warmupTicks; tick += 1) {
        server.emit("frame", makeFrame(nowMicros(), payload));
        await sleep(1000 / hz);
    }

    measuring = true;
    const ticks = seconds * hz;
    const cpuBefore = cpuMilliseconds();
    const startedAt = Date.now();
    for (let tick = 0; tick < ticks; tick += 1) {
        server.emit("frame", makeFrame(nowMicros(), payload));
        const due = startedAt + Math.round(((tick + 1) * 1000) / hz);
        const wait = due - Date.now();
        if (wait > 0) {
            await sleep(wait);
        }
    }
    await sleep(500);
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

    for (const client of clients) {
        client.close();
    }
    server.close();
    http.close();
    await sleep(200);
}

writeResult(args.out, {
    stack: "node-socketio",
    path: "socket.io 4.x, websocket transport, no compression",
    hz,
    seconds,
    payload_bytes: payloadBytes,
    sweep,
});

// Socket.IO keeps handles alive past close(); the measurement is done, so end deliberately
// rather than let the process hang looking finished.
process.exit(0);
