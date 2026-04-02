// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's framework column of the call comparison: Next.js Server Functions.
//
// The workload is the one SynQt's returning slot answers -- a caller asks the server to do
// something and waits for the value back -- and Server Functions are the Next.js feature
// that answers it in the same shape: a function the client calls, run on the server,
// returning a value. That is why this column is a Server Function and not a Route Handler.
// The Route Handler path is measured too, in the HTTP table, and the two together are the
// useful pair: they are the same server doing the same work with different framework
// machinery in front of it.
//
// Every call is the request React's client runtime makes, not a shortcut around it:
// ./serveraction.mjs reads the action id out of the build and POSTs the flight-encoded
// arguments to the page route. What is being measured therefore includes the action
// lookup, the flight decode of the arguments, the function body, and the flight encode of
// the result -- all of which a deployment pays and none of which a direct import would show.
//
// Held constant with calls-bare.mjs: the clock, the warm-up, the closed loop, the measured
// window, the statistics, and caller and server in one process.

import {
    driveCalls, parseArgs, reportCall, residentBytes, summarizeCalls, writeResult,
} from "./measure.mjs";
import {startNext} from "./nextserver.mjs";
import {actionIds, checkedAction} from "./serveraction.mjs";

const args = parseArgs({
    callers: "1,8,32,128",
    seconds: "5",
    work: "echo",
    out: "",
});

const sizes = String(args.callers).split(",")
    .map((value) => Number.parseInt(value.trim(), 10))
    .filter((value) => Number.isFinite(value) && value > 0);
const seconds = Math.max(1, Number.parseInt(args.seconds, 10));
const work = String(args.work);
if (work !== "echo" && work !== "lookup") {
    console.error(`--work is echo or lookup, not '${work}'`);
    process.exit(2);
}

console.log(`Node (Next.js Server Functions) call path: ${work}, ${seconds}s per size, `
            + `callers ${args.callers}`);

// One server for the whole sweep, as the live column does and for the same reason:
// `app.prepare()` is seconds of work and four cold starts inside one run would land in the
// numbers. What the sweep opens and closes is callers.
const {app, server} = await startNext(0);
const origin = `http://127.0.0.1:${server.address().port}`;

const ids = actionIds();
if (!ids[work]) {
    console.error(`the build names no server action '${work}' `
                  + `(it has: ${Object.keys(ids).join(", ") || "none"}); `
                  + `rebuild with: (cd benchmarks/vs-node/node/nextjs && npx next build)`);
    process.exit(1);
}

// Checked once, before anything is timed. This path is addressed by a build-assigned id
// rather than a URL, so the ways to end up measuring an error page are more numerous than
// usual, and every one of them would look fast.
const call = await checkedAction(origin, ids[work], "/", async (invoke) => {
    if (work === "echo") {
        const answer = await invoke("ping");
        if (answer !== "ping") {
            throw new Error(`echo returned ${JSON.stringify(answer)}, not "ping"`);
        }
        return;
    }
    const answer = await invoke(41);
    if (!answer || answer.id !== 42 || typeof answer.randomNumber !== "number") {
        throw new Error(`lookup returned ${JSON.stringify(answer)}`);
    }
});

const payload = work === "echo" ? "ping" : 0;
const baselineRss = residentBytes();
const sweep = [];

for (const callers of sizes) {
    const run = await driveCalls({
        callers,
        seconds,
        warmupCalls: 50,
        call: (index) => call(work === "echo" ? payload : index),
    });
    const entry = summarizeCalls({
        callers,
        ...run,
        rssPerCaller: callers > 0 ? (residentBytes() - baselineRss) / callers : 0,
        rssTotal: residentBytes(),
    });
    reportCall(entry);
    sweep.push(entry);
}

writeResult(args.out, {
    benchmark: "vs-node-calls",
    stack: "node-nextjs-action",
    path: "Next.js 16 App Router, Server Function over the React flight protocol",
    work,
    seconds,
    sweep,
});

server.close();
await app.close?.();
// Next keeps handles alive past close(); the measurement is done, so end deliberately
// rather than let the process hang looking finished. The live column ends the same way.
process.exit(0);
