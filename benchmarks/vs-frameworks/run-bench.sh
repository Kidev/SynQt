#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# SynQt against Node.js, in both directions. Builds the SynQt harnesses, runs every column
# over the same sweep, and writes one baseline each under benchmarks/results/ keyed by
# hostname. Pinned Qt 6.11.1.
#
#   ./run-bench.sh
#   ./run-bench.sh --subscribers 10,50,100,250,500 --seconds 10 --hz 60
#
# Two tables come out of it. The live one first, which is the headline: one publisher, N
# subscribers, and the arguments above shape it. Then the call one: a caller asks and waits,
# which is the direction a Next.js Server Function goes, shaped by CALL_CALLERS,
# CALL_SECONDS and CALL_WORK instead (the two sweeps count different things, so one set of
# flags cannot mean the same thing to both).
#
# QT_HOST overrides the kit path and BENCH_OUT_DIR overrides where the baselines are
# written, so CI can run this against its own kit without writing into benchmarks/results/.
#
# The HTTP half is a separate run. Those servers are driven by the loader in
# benchmarks/edge, which measures request throughput rather than propagation. See
# benchmarks/vs-frameworks/README.md.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-vs-frameworks"
# Resolved to an absolute path once, here, because the columns are run from their own
# directories. A relative results directory means something different inside
# benchmarks/vs-frameworks/node/ than it does at the repository root. Prefixing it
# with $REPO_ROOT at each Node column instead produces `/repo//abs/path` and a
# no-such-file the moment BENCH_OUT_DIR is given an absolute one.
RESULTS_DIR="${BENCH_OUT_DIR:-$REPO_ROOT/benchmarks/results}"
mkdir -p "$RESULTS_DIR"
RESULTS_DIR="$(cd "$RESULTS_DIR" && pwd)"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
NODE_DIR="benchmarks/vs-frameworks/node"

echo "== configure + build the SynQt column =="
cmake -S benchmarks/vs-frameworks -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

if [ ! -d "$NODE_DIR/node_modules" ]; then
    echo "== install the Node columns' dependencies =="
    # Only the two framework columns need these. The bare column is Node built-ins by
    # definition, and it runs whether or not this succeeded.
    (cd "$NODE_DIR" && npm install --no-audit --no-fund)
fi

# Next.js in production is a build, not a flag: a server started against no build answers 404
# for every route it was going to be measured on. Rebuilt whenever a route is newer than the
# build, so editing one is not a run that silently measured the previous version.
if [ ! -f "$NODE_DIR/nextjs/.next/BUILD_ID" ] || \
   [ -n "$(find "$NODE_DIR/nextjs/app" "$NODE_DIR/techempower.mjs" \
                -newer "$NODE_DIR/nextjs/.next/BUILD_ID" -print -quit 2>/dev/null)" ]; then
    echo "== build the Next.js column =="
    (cd "$NODE_DIR/nextjs" && npx next build)
fi

echo
echo "== SynQt =="
"$BUILD_DIR/bench_live" --out "$RESULTS_DIR/vs-fw-synqt-${HOST_TAG}.json" "$@"

echo
echo "== Qt, bare QWebSocket (the same fan-out with no object protocol on it) =="
"$BUILD_DIR/bench_live" --raw --out "$RESULTS_DIR/vs-fw-qtraw-${HOST_TAG}.json" "$@"

echo
echo "== Node, bare (node:http + hand-rolled RFC 6455) =="
(cd "$NODE_DIR" && node live-bare.mjs \
    --out "$RESULTS_DIR/vs-fw-bare-${HOST_TAG}.json" "$@")

echo
echo "== Node, realistic (Socket.IO) =="
(cd "$NODE_DIR" && node live-socketio.mjs \
    --out "$RESULTS_DIR/vs-fw-socketio-${HOST_TAG}.json" "$@")

echo
echo "== Node, framework (Next.js, server-sent events) =="
(cd "$NODE_DIR" && node live-nextjs.mjs \
    --out "$RESULTS_DIR/vs-fw-nextjs-${HOST_TAG}.json" "$@")

echo
echo "== the table =="
python3 benchmarks/vs-frameworks/compare.py "$RESULTS_DIR"/vs-fw-*-"${HOST_TAG}".json

# The other direction. A caller asks and waits. This is where the Next.js Server Function
# column lives, because that is the Next.js feature shaped like a connect point's returning
# slot.
#
# Its own flags rather than "$@": the live columns sweep subscribers and these sweep
# callers, so forwarding one run's arguments to the other would hand --subscribers to a
# program that has no such option. CALL_CALLERS, CALL_SECONDS and CALL_WORK are the knobs.
#
# Written under vs-call- rather than vs-fw-calls-, so the live table's glob above keeps
# matching only the live results.
CALL_WORK="${CALL_WORK:-echo}"
CALL_CALLERS="${CALL_CALLERS:-1,8,32,128}"
CALL_SECONDS="${CALL_SECONDS:-5}"
CALL_ARGS=(--callers "$CALL_CALLERS" --seconds "$CALL_SECONDS" --work "$CALL_WORK")

echo
echo "== SynQt, a connect point's returning slot ($CALL_WORK) =="
"$BUILD_DIR/bench_call" "${CALL_ARGS[@]}" \
    --out "$RESULTS_DIR/vs-call-synqt-${HOST_TAG}.json"

echo
echo "== Node, bare (node:http, a JSON body each way) =="
(cd "$NODE_DIR" && node calls-bare.mjs "${CALL_ARGS[@]}" \
    --out "$RESULTS_DIR/vs-call-bare-${HOST_TAG}.json")

echo
echo "== Node, framework (Next.js Server Functions) =="
(cd "$NODE_DIR" && node calls-nextjs.mjs "${CALL_ARGS[@]}" \
    --out "$RESULTS_DIR/vs-call-nextjs-${HOST_TAG}.json")

echo
echo "== the call table =="
python3 benchmarks/vs-frameworks/compare-calls.py "$RESULTS_DIR"/vs-call-*-"${HOST_TAG}".json
