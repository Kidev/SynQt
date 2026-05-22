// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The measurement half, shared by both Node columns so the only difference between them is
// the server. It is the same shape as the SynQt harness reports
// (benchmarks/vs-frameworks/bench_live.cpp), the same distribution summary, the same frame layout
// (8 bytes of microsecond stamp, then payload), the same delivered/expected pair, and the
// same JSON. Two columns that measured differently would not be a comparison.

import { writeFileSync } from "node:fs";
import { Agent, request as httpRequest } from "node:http";
import { arch, platform, release } from "node:os";

export function distribution(samples) {
    const sorted = [...samples].sort((a, b) => a - b);
    const at = (fraction) => {
        if (sorted.length === 0) {
            return 0;
        }
        const rank = fraction * (sorted.length - 1);
        const low = Math.floor(rank);
        const high = Math.ceil(rank);
        if (low === high) {
            return sorted[low];
        }
        return sorted[low] + (rank - low) * (sorted[high] - sorted[low]);
    };
    const mean = sorted.length
        ? sorted.reduce((total, value) => total + value, 0) / sorted.length
        : 0;
    return {
        unit: "ms",
        samples: sorted.length,
        min: sorted.length ? sorted[0] : 0,
        p50: at(0.5),
        p95: at(0.95),
        p99: at(0.99),
        max: sorted.length ? sorted[sorted.length - 1] : 0,
        mean,
    };
}

/// Microseconds since this process started, on the monotonic clock. The same clock reads
/// the stamp back, so what is measured is an interval and never a wall-clock difference.
export function nowMicros() {
    return Number(process.hrtime.bigint() / 1000n);
}

export function makeFrame(stampMicros, payload) {
    const frame = Buffer.allocUnsafe(8 + payload.length);
    frame.writeBigUInt64LE(BigInt(stampMicros), 0);
    payload.copy(frame, 8);
    return frame;
}

export function readStamp(frame) {
    return Number(frame.readBigUInt64LE(0));
}

export function residentBytes() {
    return process.memoryUsage().rss;
}

/// Process CPU in milliseconds, user plus system, to compare against the C++ side's
/// std::clock. Both are "CPU this process burned", which is the figure a host is sized on.
export function cpuMilliseconds() {
    const usage = process.cpuUsage();
    return (usage.user + usage.system) / 1000;
}

export function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export function parseArgs(defaults) {
    const values = { ...defaults };
    const argv = process.argv.slice(2);
    for (let i = 0; i < argv.length; i += 1) {
        const flag = argv[i];
        if (!flag.startsWith("--")) {
            continue;
        }
        const name = flag.slice(2);
        const value = argv[i + 1];
        if (value === undefined || value.startsWith("--")) {
            values[name] = true;
            continue;
        }
        values[name] = value;
        i += 1;
    }
    return values;
}

/// One HTTP client for every call column, over `node:http` with a keep-alive agent.
///
/// Not the global `fetch`, which would stop the control being a control. `fetch` is undici,
/// a client library whose per-call cost varies across Node releases by more than the
/// servers under test differ, while `node:http` stays flat; a table about server frameworks
/// would report a difference between client libraries.
/// difference between client libraries.
///
/// The agent matters as much as the module. A caller here holds its connection for the
/// length of the run, which is what the SynQt column does and what the Next.js column's
/// real client does, so a new socket per call would be measuring connection setup. And
/// `maxSockets` is unbounded because the sweep's whole axis is concurrent callers, and
/// a pool smaller than the caller count would queue them on the client and report the
/// queueing as the server's latency.
export function httpCaller({url, headers, decode}) {
    const agent = new Agent({keepAlive: true, maxSockets: Infinity});
    return (body) => new Promise((resolve, reject) => {
        const request = httpRequest(url, {method: "POST", headers, agent}, (response) => {
            let text = "";
            response.setEncoding("utf8");
            response.on("data", (chunk) => { text += chunk; });
            response.on("end", () => {
                if (response.statusCode !== 200) {
                    reject(new Error(`the call answered ${response.statusCode}`));
                    return;
                }
                resolve(decode(text));
            });
        });
        request.on("error", reject);
        request.end(body);
    });
}

/// The stack id a Node column records itself under, with the runtime major in it.
///
/// The comparison measures Node twice, on the active LTS and on the current release, so the
/// id has to say which one produced a row. Two runs under one name is the second overwriting
/// the first, and a table cannot show a difference it has no way to name. The major only,
/// because the exact version is already in `node_version` beside it and a stack id that
/// moved with every patch would orphan every committed baseline.
export function nodeStack(name) {
    return `node${process.versions.node.split(".")[0]}-${name}`;
}

export function writeResult(path, root) {
    const complete = {
        benchmark: "vs-frameworks-live",
        node_version: process.version,
        host: `${platform()} ${release()}`,
        arch: arch(),
        recorded: new Date().toISOString(),
        rss_available: true,
        ...root,
    };
    if (path) {
        writeFileSync(path, `${JSON.stringify(complete, null, 2)}\n`);
        console.log(`\nwrote ${path}`);
    }
    return complete;
}

/// One sweep entry, computed identically on both Node columns.
export function summarize({ subscribers, propagation, delivered, expected, elapsedSeconds,
                            cpuMs, rssPerConnection, rssTotal }) {
    return {
        subscribers,
        propagation: distribution(propagation),
        throughput_msgs_per_sec: delivered / Math.max(elapsedSeconds, 0.001),
        cpu_ms_per_1k: delivered > 0 ? (cpuMs * 1000) / delivered : 0,
        rss_bytes_per_conn: rssPerConnection,
        rss_total_bytes: rssTotal,
        delivered,
        expected,
    };
}

export function report(entry) {
    const p = entry.propagation;
    console.log(
        `  N=${entry.subscribers}` +
        `  p50 ${p.p50.toFixed(3)} ms` +
        `  p99 ${p.p99.toFixed(3)} ms` +
        `  ${entry.throughput_msgs_per_sec.toFixed(0)} msg/s` +
        `  delivered ${entry.delivered}/${entry.expected}`);
}

/// One sweep entry for the call comparison, computed identically on every column of it.
///
/// Separate from summarize() above because the two tables answer different questions and
/// share only their statistics. The live one is "one change, N subscribers see it" and its
/// unit is a delivery, this one is "a caller asks and waits" and its unit is a round trip.
/// Reusing the live shape would put a `subscribers` count on a table that has none and call
/// a latency a propagation.
export function summarizeCalls({ callers, latency, completed, failed, elapsedSeconds, cpuMs,
                                 rssPerCaller, rssTotal }) {
    return {
        callers,
        latency: distribution(latency),
        throughput_calls_per_sec: completed / Math.max(elapsedSeconds, 0.001),
        cpu_ms_per_1k: completed > 0 ? (cpuMs * 1000) / completed : 0,
        rss_bytes_per_caller: rssPerCaller,
        rss_total_bytes: rssTotal,
        completed,
        failed,
    };
}

export function reportCall(entry) {
    const l = entry.latency;
    console.log(
        `  callers=${entry.callers}` +
        `  p50 ${l.p50.toFixed(3)} ms` +
        `  p99 ${l.p99.toFixed(3)} ms` +
        `  ${entry.throughput_calls_per_sec.toFixed(0)} calls/s` +
        `  failed ${entry.failed}`);
}

/// Drive one column of the call comparison. `callers` callers, each with exactly one call in
/// flight, for `seconds`, and the statistics over what came back.
///
/// Closed loop per caller, and that is the whole design. Firing calls open-loop at a fixed
/// rate would measure the queue in front of the server rather than what the server does, and
/// the concurrency would be whatever the rate happened to outrun. Here the concurrency is
/// the thing being swept and it is exact: N callers, N calls outstanding, never N+1.
///
/// `call(index)` makes one call and resolves when its answer is back. It is the only thing
/// that differs between the columns. Everything above it is shared, so a difference in the
/// table is a difference in the stack and not in the harness.
export async function driveCalls({ call, callers, seconds, warmupCalls }) {
    const latency = [];
    let completed = 0;
    let failed = 0;
    let measuring = false;
    let running = true;

    const once = async (index) => {
        const started = nowMicros();
        try {
            await call(index);
        } catch (error) {
            if (measuring) {
                failed += 1;
            }
            return;
        }
        if (measuring) {
            latency.push((nowMicros() - started) / 1000);
            completed += 1;
        }
    };

    // Warm up on one caller rather than all of them. The first calls pay for lazily built
    // route tables, a first database statement and a first TLS-less socket, and what is being
    // warmed is the server, which every caller shares.
    for (let i = 0; i < warmupCalls; i += 1) {
        await once(0);
    }

    const loops = [];
    measuring = true;
    const cpuBefore = cpuMilliseconds();
    const startedAt = Date.now();
    for (let index = 0; index < callers; index += 1) {
        loops.push((async () => {
            while (running) {
                await once(index);
            }
        })());
    }
    // The window is wall-clock and the loops end on the flag, so a call already in flight
    // when time runs out is awaited rather than abandoned. Abandoning it would report a
    // throughput over calls whose latency was never counted.
    await sleep(seconds * 1000);
    running = false;
    await Promise.all(loops);

    return {
        latency,
        completed,
        failed,
        elapsedSeconds: (Date.now() - startedAt) / 1000,
        cpuMs: cpuMilliseconds() - cpuBefore,
    };
}
