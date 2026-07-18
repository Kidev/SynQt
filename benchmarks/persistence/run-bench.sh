#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build and run the persistence + cache baseline (M9), writing a JSON baseline under
# benchmarks/results/ keyed by hostname. Pinned Qt 6.11.1. Pass extra flags through, e.g.
#   ./run-bench.sh --batched-rows 200000 --reads 100000

set -euo pipefail

QT_HOST="/opt/Qt/6.11.1/gcc_64"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-persistence"
RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
OUT="${RESULTS_DIR}/persistence-${HOST_TAG}.json"

echo "== configure + build =="
cmake -S benchmarks/persistence -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

echo "== run =="
mkdir -p "$RESULTS_DIR"
"$BUILD_DIR/bench_persistence" --out "$OUT" "$@"
