// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Drive the TechEmpower test types against the bench edge and write a committed baseline.
// Load is generated with a dependency-free, keep-alive HTTP loader (Node builtins only): for
// each cell it holds `connections` concurrent request loops open for the measured window,
// records every request's latency, and reports requests/sec plus the latency distribution
// (p50/p90/p99). What makes the numbers comparable across frameworks is the test types and
// the methodology; warm up, sweep the connection count, report throughput and percentiles --
// the same shape TechEmpower uses with wrk; this ships no external dependency so it runs
// anywhere the edge builds. The driver owns the server's lifecycle (spawn, wait, kill) so the
// whole run is one process with no shell job control. Output matches results/transport-*.json.

import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import http from "node:http";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const base = process.env.BENCH_URL || "http://127.0.0.1:8480";
const outDir = path.resolve(here, "../../results");
const port = Number(new URL(base).port || 8480);
const host = new URL(base).hostname;
const edgeBin = process.env.BENCH_EDGE
    || path.resolve(here, "../../../build/bench-edge/bench-edge");

const WARMUP_SECONDS = Number(process.env.BENCH_WARMUP || 2);
const MEASURE_SECONDS = Number(process.env.BENCH_MEASURE || 6);
const CONNECTIONS = (process.env.BENCH_CONNECTIONS || "16,64,256")
    .split(",").map((value) => Number(value.trim())).filter((value) => value > 0);
const QUERY_COUNT = 20; // the queries=/updates= count; TechEmpower sweeps 1..500, 20 is a mid point.

const TESTS = [
    { name: "plaintext", path: "/plaintext" },
    { name: "json", path: "/json" },
    { name: "single_query", path: "/db" },
    { name: "multiple_queries", path: `/queries?queries=${QUERY_COUNT}` },
    { name: "updates", path: `/updates?queries=${QUERY_COUNT}` },
    { name: "fortunes", path: "/fortunes" }
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function request(agent, target) {
    return new Promise((resolve) => {
        const start = process.hrtime.bigint();
        const req = http.get(target, { agent }, (res) => {
            let bytes = 0;
            res.on("data", (chunk) => {
                bytes += chunk.length;
            });
            res.on("end", () => {
                const ms = Number(process.hrtime.bigint() - start) / 1e6;
                resolve({ ok: res.statusCode === 200, ms, bytes });
            });
        });
        req.on("error", () => resolve({ ok: false, ms: 0, bytes: 0 }));
    });
}

// One worker keeps a single keep-alive connection busy: fire, await, repeat until the
// deadline. `connections` of them in parallel is the offered concurrency.
async function worker(target, deadline, sink) {
    const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
    while (performance.now() < deadline) {
        const result = await request(agent, target);
        sink.latencies.push(result.ms);
        sink.bytes += result.bytes;
        if (!result.ok) {
            sink.errors += 1;
        }
    }
    agent.destroy();
}

async function load(target, connections, seconds) {
    const sink = { latencies: [], bytes: 0, errors: 0 };
    const deadline = performance.now() + seconds * 1000;
    const workers = [];
    for (let i = 0; i < connections; i++) {
        workers.push(worker(target, deadline, sink));
    }
    await Promise.all(workers);
    return sink;
}

function percentile(sorted, p) {
    if (sorted.length === 0) {
        return 0;
    }
    const rank = Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length));
    return Number(sorted[rank].toFixed(3));
}

async function measure(test, connections) {
    const target = base + test.path;
    await load(target, connections, WARMUP_SECONDS); // warm up, discard
    const sink = await load(target, connections, MEASURE_SECONDS);
    const sorted = sink.latencies.slice().sort((a, b) => a - b);
    const total = sorted.length;
    const mean = total ? sorted.reduce((a, b) => a + b, 0) / total : 0;
    return {
        test: test.name,
        path: test.path,
        connections,
        requests_per_sec: Math.round(total / MEASURE_SECONDS),
        throughput_bytes_per_sec: Math.round(sink.bytes / MEASURE_SECONDS),
        latency_ms: {
            p50: percentile(sorted, 50),
            p90: percentile(sorted, 90),
            p99: percentile(sorted, 99),
            mean: Number(mean.toFixed(3)),
            max: total ? Number(sorted[total - 1].toFixed(3)) : 0
        },
        errors: sink.errors
    };
}

function ping() {
    return new Promise((resolve) => {
        const req = http.get(base + "/plaintext", (res) => {
            res.resume();
            resolve(res.statusCode === 200);
        });
        req.on("error", () => resolve(false));
        req.setTimeout(500, () => {
            req.destroy();
            resolve(false);
        });
    });
}

async function startEdge() {
    if (!fs.existsSync(edgeBin)) {
        throw new Error(`bench edge not built at ${edgeBin} (run the build step first)`);
    }
    const child = spawn(edgeBin, ["--port", String(port)], { stdio: ["ignore", "pipe", "pipe"] });
    child.stdout.on("data", (d) => process.stdout.write("[edge] " + d));
    child.stderr.on("data", (d) => process.stdout.write("[edge] " + d));
    for (let i = 0; i < 60; i++) {
        if (await ping()) {
            return child;
        }
        await sleep(250);
    }
    child.kill("SIGKILL");
    throw new Error("bench edge did not answer within 15s");
}

function hostLabel() {
    try {
        const osRelease = fs.readFileSync("/etc/os-release", "utf8");
        const m = osRelease.match(/^PRETTY_NAME="?([^"\n]+)"?/m);
        return m ? m[1] : os.type();
    } catch {
        return os.type();
    }
}

async function main() {
    const spawnEdge = process.env.BENCH_SPAWN !== "0";
    const edge = spawnEdge ? await startEdge() : null;
    const results = [];
    try {
        for (const test of TESTS) {
            for (const connections of CONNECTIONS) {
                process.stdout.write(`  ${test.name} @ ${connections} conn ... `);
                const row = await measure(test, connections);
                results.push(row);
                console.log(`${row.requests_per_sec.toLocaleString()} req/s  ` +
                    `p50=${row.latency_ms.p50}ms p90=${row.latency_ms.p90}ms ` +
                    `p99=${row.latency_ms.p99}ms` +
                    (row.errors ? `  (errors=${row.errors})` : ""));
            }
        }
    } finally {
        if (edge) {
            edge.kill("SIGKILL");
        }
    }

    const payload = {
        benchmark: "edge-http",
        suite: "techempower",
        tool: "node-keepalive-loader",
        note: "TechEmpower test types on SynQt's QHttpServer + QSQLITE edge stack. "
            + "Load: dependency-free keep-alive loader; methodology matches TechEmpower/wrk "
            + "(warm up, sweep connections, report req/s and latency percentiles).",
        qt_version: "6.11.1",
        host: hostLabel(),
        target_host: host,
        arch: os.arch(),
        cpus: os.cpus().length,
        connections_swept: CONNECTIONS,
        warmup_seconds: WARMUP_SECONDS,
        measure_seconds: MEASURE_SECONDS,
        query_count: QUERY_COUNT,
        generated: new Date().toISOString(),
        results
    };

    fs.mkdirSync(outDir, { recursive: true });
    const hostSlug = (os.hostname() || "host").replace(/[^A-Za-z0-9]+/g, "");
    const outFile = path.join(outDir, `edge-http-${hostSlug}.json`);
    fs.writeFileSync(outFile, JSON.stringify(payload, null, 4) + "\n");
    console.log(`\nwrote ${path.relative(path.resolve(here, "../../.."), outFile)}`);
}

main().catch((err) => {
    console.error("http-bench crashed:", err);
    process.exit(1);
});
