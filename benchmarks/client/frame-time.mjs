// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Drive a built WebAssembly client scene in a real browser and measure two things the plan asks
// for: cold start (time from navigation to the first rendered frame) and frame time as the number
// of entities in view grows. The scene ramps its blob count and prints "BENCH blobs=N frameMs=X"
// per batch through qWarning (QML console.log is not reliable in a WASM release build); this
// harness collects those lines, buckets them by blob count, and reports p50/p95/p99 per bucket so
// the render-cost curve is explicit. The page is served under COOP/COEP so the multi-threaded kit
// gets its SharedArrayBuffer; the headers are harmless for the single-threaded kit.
//
//   node frame-time.mjs --dir <bundle-dir> --label <name> --out <file.json> [--bucket 25]

import { chromium } from "playwright";
import http from "node:http";
import fsp from "node:fs/promises";
import path from "node:path";
import process from "node:process";

function parseArgs(argv) {
    const args = { dir: "", label: "scene", out: "", bucket: 25, headless: true };
    for (let i = 0; i < argv.length; i += 1) {
        const key = argv[i];
        if (key === "--dir") { args.dir = argv[++i]; }
        else if (key === "--label") { args.label = argv[++i]; }
        else if (key === "--out") { args.out = argv[++i]; }
        else if (key === "--bucket") { args.bucket = Number(argv[++i]); }
        else if (key === "--headed") { args.headless = false; }
    }
    if (!args.dir) {
        console.error("frame-time: --dir <bundle-dir> is required");
        process.exit(2);
    }
    return args;
}

const MIME = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".wasm": "application/wasm",
    ".json": "application/json",
    ".data": "application/octet-stream",
    ".svg": "image/svg+xml",
    ".css": "text/css",
    ".ico": "image/x-icon",
    ".png": "image/png"
};

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Serve the bundle with the cross-origin-isolation headers the threaded runtime needs. The same
// headers the SynQt web edge emits when security.cross_origin_isolation is on.
async function startServer(dir, entryHtml) {
    const server = http.createServer(async (req, res) => {
        try {
            const parsed = new URL(req.url, "http://127.0.0.1");
            let rel = decodeURIComponent(parsed.pathname);
            if (rel === "/") {
                rel = "/" + entryHtml;
            }
            const file = path.join(dir, rel);
            if (!file.startsWith(dir)) {
                res.writeHead(403);
                res.end();
                return;
            }
            const data = await fsp.readFile(file);
            const ext = path.extname(file).toLowerCase();
            res.writeHead(200, {
                "Content-Type": MIME[ext] || "application/octet-stream",
                "Cross-Origin-Opener-Policy": "same-origin",
                "Cross-Origin-Embedder-Policy": "require-corp"
            });
            res.end(data);
        } catch (err) {
            res.writeHead(404);
            res.end(String(err));
        }
    });
    return new Promise((resolve) => {
        server.listen(0, "127.0.0.1", () => resolve(server));
    });
}

async function findEntryHtml(dir) {
    const names = await fsp.readdir(dir);
    const html = names.filter((name) => name.endsWith(".html"));
    if (html.length === 0) {
        throw new Error(`no .html entry in ${dir}`);
    }
    // Prefer the app page over a generic index if both exist.
    return html.find((name) => name !== "index.html") || html[0];
}

function percentile(sorted, fraction) {
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
}

function bucketize(samples, bucketSize) {
    const groups = new Map();
    for (const sample of samples) {
        const key = Math.max(1, Math.round(sample.blobs / bucketSize) * bucketSize);
        if (!groups.has(key)) {
            groups.set(key, []);
        }
        groups.get(key).push(sample.frameMs);
    }
    const buckets = [];
    for (const [blobs, values] of [...groups.entries()].sort((a, b) => a[0] - b[0])) {
        const sorted = [...values].sort((a, b) => a - b);
        const mean = sorted.reduce((total, value) => total + value, 0) / sorted.length;
        buckets.push({
            blobs,
            count: sorted.length,
            p50: percentile(sorted, 0.5),
            p95: percentile(sorted, 0.95),
            p99: percentile(sorted, 0.99),
            mean,
            fps_p50: 1000 / percentile(sorted, 0.5)
        });
    }
    return buckets;
}

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const dir = path.resolve(args.dir);
    const entryHtml = await findEntryHtml(dir);
    const server = await startServer(dir, entryHtml);
    const port = server.address().port;

    const browser = await chromium.launch({
        headless: args.headless,
        args: [
            "--use-gl=angle",
            "--use-angle=swiftshader",
            "--enable-unsafe-swiftshader",
            "--ignore-gpu-blocklist"
        ]
    });

    const samples = [];
    let firstFrameAt = null;
    let done = false;
    const navStart = Date.now();

    try {
        const context = await browser.newContext();
        const page = await context.newPage();
        page.on("console", (msg) => {
            const text = msg.text();
            const match = text.match(/BENCH blobs=(\d+) frameMs=([\d.]+)/);
            if (match) {
                if (firstFrameAt === null) {
                    firstFrameAt = Date.now();
                }
                samples.push({ blobs: Number(match[1]), frameMs: Number(match[2]) });
                return;
            }
            if (/BENCH done/.test(text)) {
                done = true;
            }
        });
        page.on("pageerror", (err) => console.error("PAGEERROR", err.message));

        await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: "domcontentloaded",
            timeout: 60000 });

        // Wait for the ramp to finish (BENCH done) or a hard ceiling, then take a short tail so the
        // top blob count is well sampled.
        const deadline = Date.now() + 60000;
        while (!done && Date.now() < deadline) {
            await sleep(250);
        }
        await sleep(1500);

        const isolated = await page.evaluate(() => globalThis.crossOriginIsolated === true);
        const coldStartMs = firstFrameAt === null ? null : firstFrameAt - navStart;
        const buckets = bucketize(samples, args.bucket);

        const result = {
            benchmark: "client-frame-time",
            label: args.label,
            entry: entryHtml,
            cross_origin_isolated: isolated,
            cold_start_ms: coldStartMs,
            frame_samples: samples.length,
            buckets
        };

        console.error(`== frame time: ${args.label} ` +
            `(isolated=${isolated}, cold_start=${coldStartMs}ms, samples=${samples.length}) ==`);
        for (const bucket of buckets) {
            console.error(`  blobs~${String(bucket.blobs).padStart(5)}  ` +
                `p50=${bucket.p50.toFixed(2)}ms  p95=${bucket.p95.toFixed(2)}ms  ` +
                `fps~${bucket.fps_p50.toFixed(0)}  (n=${bucket.count})`);
        }

        if (args.out) {
            await fsp.writeFile(args.out, JSON.stringify(result, null, 2));
            console.error(`wrote ${args.out}`);
        } else {
            console.log(JSON.stringify(result, null, 2));
        }

        if (samples.length === 0) {
            console.error("frame-time: no BENCH samples seen (did the scene render?)");
            process.exitCode = 1;
        }
    } finally {
        await browser.close();
        server.close();
    }
}

main().catch((err) => {
    console.error("frame-time crashed:", err);
    process.exit(2);
});
