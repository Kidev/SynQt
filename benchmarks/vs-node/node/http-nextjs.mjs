// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Node's framework column of the HTTP table: Next.js Route Handlers over better-sqlite3.
//
// Same six TechEmpower test types, same answers, same seed as http-bare.mjs, http-fastify.mjs
// and benchmarks/edge/bench_edge.cpp: the routes read ../techempower.mjs, which is the one
// description all of them answer out of, so a difference between the columns can only be
// the framework and the driver.
//
// It needs a build first, because that is what running Next.js in production is:
//
//   (cd benchmarks/vs-node/node/nextjs && npx next build)
//
// run-bench.sh does it. Serves until killed; the loader in benchmarks/edge is what measures.

import {parseArgs} from "./measure.mjs";
import {startNext} from "./nextserver.mjs";

const args = parseArgs({port: "8483"});
const {port} = await startNext(Number.parseInt(args.port, 10));
console.log(`node-http-nextjs listening on http://127.0.0.1:${port}`);
