#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The live-path comparison: SynQt against Node.js, on the workload SynQt is for. Builds the
# SynQt harness, runs all three columns over the same sweep, and writes one baseline each
# under benchmarks/results/ keyed by hostname. Pinned Qt 6.11.1.
#
#   ./run-bench.sh
#   ./run-bench.sh --subscribers 10,50,100,250,500 --seconds 10 --hz 60
#
# QT_HOST overrides the kit path and BENCH_OUT_DIR overrides where the baselines are
# written, so CI can run this against its own kit without writing into benchmarks/results/.
#
# The HTTP half is a separate run: those servers are driven by the loader in
# benchmarks/edge, which measures request throughput rather than propagation. See
# benchmarks/vs-node/README.md.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-vs-node"
RESULTS_DIR="${BENCH_OUT_DIR:-benchmarks/results}"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
NODE_DIR="benchmarks/vs-node/node"

echo "== configure + build the SynQt column =="
cmake -S benchmarks/vs-node -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

if [ ! -d "$NODE_DIR/node_modules" ]; then
    echo "== install the Node columns' dependencies =="
    # Only the realistic column needs these. The bare column is Node built-ins, which is the
    # whole point of it, and it runs whether or not this succeeded.
    (cd "$NODE_DIR" && npm install --no-audit --no-fund)
fi

mkdir -p "$RESULTS_DIR"

echo
echo "== SynQt =="
"$BUILD_DIR/bench_live" --out "$RESULTS_DIR/vs-node-synqt-${HOST_TAG}.json" "$@"

echo
echo "== Node, bare (node:http + hand-rolled RFC 6455) =="
(cd "$NODE_DIR" && node live-bare.mjs \
    --out "$REPO_ROOT/$RESULTS_DIR/vs-node-bare-${HOST_TAG}.json" "$@")

echo
echo "== Node, realistic (Socket.IO) =="
(cd "$NODE_DIR" && node live-socketio.mjs \
    --out "$REPO_ROOT/$RESULTS_DIR/vs-node-socketio-${HOST_TAG}.json" "$@")

echo
echo "== the table =="
python3 benchmarks/vs-node/compare.py "$RESULTS_DIR"/vs-node-*-"${HOST_TAG}".json
