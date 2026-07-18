#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The web edge's HTTP path, measured with the TechEmpower test types so the numbers compare
# to other web frameworks. Builds the bench edge (QHttpServer + QSQLITE, the edge's own
# stack), then a dependency-free Node loader spawns it and sweeps the connection count over
# plaintext / json / single-query / multiple-queries / updates / fortunes, writing a committed
# baseline under benchmarks/results/. Pins Qt 6.11.1; records host, arch, and Qt.

set -euo pipefail

QT_HOST="/opt/Qt/6.11.1/gcc_64"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT="${BENCH_PORT:-8480}"
cd "$REPO_ROOT"

echo "== [1/2] Build the bench edge (native, QHttpServer + QSQLITE) =="
cmake -S benchmarks/edge -B build/bench-edge -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=Release
cmake --build build/bench-edge

echo "== [2/2] Run the TechEmpower matrix (warm up, sweep connections, record percentiles) =="
# The driver (Node builtins only, no external tool) spawns the built edge, waits for it, runs
# the matrix, and kills it; one process, no shell job control. Override BENCH_MEASURE /
# BENCH_CONNECTIONS for a shorter run; BENCH_SPAWN=0 to drive an already-running edge.
cd benchmarks/edge/verify
BENCH_URL="http://127.0.0.1:$PORT" node run-http-bench.mjs
