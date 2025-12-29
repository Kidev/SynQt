// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The measurement half, shared by both Node columns so the only difference between them is
// the server. Deliberately the same shape as the SynQt harness reports
// (benchmarks/vs-node/bench_live.cpp): the same distribution summary, the same frame layout
// (8 bytes of microsecond stamp, then payload), the same delivered/expected pair, and the
// same JSON. Two columns that measured differently would not be a comparison.

import { writeFileSync } from "node:fs";
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

export function writeResult(path, root) {
    const complete = {
        benchmark: "vs-node-live",
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
