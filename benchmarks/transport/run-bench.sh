#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build and run the client-to-edge transport baseline, writing a JSON baseline under
# benchmarks/results/ keyed by hostname so a later run on the same runner can be diffed
# against it. Pinned Qt 6.11.1. Pass extra flags through to the harness, e.g.
#   ./run-bench.sh --samples 5000 --throughput-calls 50000

set -euo pipefail

QT_HOST="/opt/Qt/6.11.1/gcc_64"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-transport"
RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
OUT="${RESULTS_DIR}/transport-${HOST_TAG}.json"

echo "== configure + build =="
cmake -S benchmarks/transport -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

echo "== run =="
mkdir -p "$RESULTS_DIR"
"$BUILD_DIR/bench_transport" --out "$OUT" "$@"
