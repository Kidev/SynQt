#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build and run the monitoring pipeline baseline, writing a JSON baseline under
# benchmarks/results/ keyed by hostname. Pinned Qt 6.12.0. Extra flags pass through, e.g.
#   ./run-bench.sh --batches 400 --batch-size 10000
#
# QT_HOST overrides the kit path and BENCH_OUT overrides where the baseline is written,
# so CI can run this against its own kit without writing into benchmarks/results/.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-monitor"
RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
OUT="${BENCH_OUT:-${RESULTS_DIR}/monitor-${HOST_TAG}.json}"

echo "== configure + build =="
cmake -S benchmarks/monitor -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

echo "== run =="
mkdir -p "$RESULTS_DIR"
"$BUILD_DIR/bench_pipeline" --out "$OUT" "$@"
