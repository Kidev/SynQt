#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build and run the capstone arena load test (the scaling scenario), writing a JSON baseline under
# benchmarks/results/ keyed by hostname. Pinned Qt 6.11.1. It sustains a fixed-rate server loop and
# many live connections, so it belongs on a real host, not the build sandbox. Pass extra flags
# through to the harness, e.g.
#   ./run-bench.sh --sizes 10,50,100,250,500 --hz 30 --seconds 8 --interest 16

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-capstone"
RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
OUT="${RESULTS_DIR}/capstone-${HOST_TAG}.json"

echo "== configure + build =="
cmake -S benchmarks/capstone -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

echo "== run =="
mkdir -p "$RESULTS_DIR"
"$BUILD_DIR/bench_capstone" --out "$OUT" "$@"
