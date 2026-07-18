#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build the client frame-time scene for both WebAssembly kits, weigh each bundle (raw / gzip /
# brotli), then drive it in a real browser to record cold start and frame time as the number of
# entities in view grows. Writes JSON baselines under benchmarks/results/ keyed by kit and
# hostname. It needs a real display and the WASM kits, so it belongs on a workstation, not the
# build sandbox; the single-threaded kit is enough for a first baseline and the multi-threaded kit
# is measured too when present.
#
#   benchmarks/client/run-bench.sh [--blobs 2000] [--ramp 15]

set -euo pipefail

QT_WASM_ST="${QT_WASM_ST:-/opt/Qt/6.11.1/wasm_singlethread}"
QT_WASM_MT="${QT_WASM_MT:-/opt/Qt/6.11.1/wasm_multithread}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$REPO_ROOT/benchmarks/client"
cd "$REPO_ROOT"

BLOBS=2000
RAMP=15
while [ $# -gt 0 ]; do
    case "$1" in
        --blobs) BLOBS="$2"; shift 2 ;;
        --ramp) RAMP="$2"; shift 2 ;;
        *) echo "run-bench: unknown arg $1" >&2; exit 2 ;;
    esac
done

RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
mkdir -p "$RESULTS_DIR"

echo "== [1/3] Install the browser driver =="
cd "$HERE"
npm install --no-audit --no-fund
npx --yes playwright install chromium
cd "$REPO_ROOT"

# Build one kit, weigh it, and drive it. $1 = kit label, $2 = qt-cmake path, $3.. = extra cmake args.
build_measure_drive() {
    local kit="$1"; shift
    local qtcmake="$1"; shift
    local build_dir="build/bench-client-wasm-${kit}"

    echo "== build scene (${kit}) =="
    "$qtcmake" -S benchmarks/client/scene -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSYNQT_BENCH_BLOBS="$BLOBS" \
        -DSYNQT_BENCH_RAMP="$RAMP" "$@"
    cmake --build "$build_dir"

    echo "== weigh bundle (${kit}) =="
    bash benchmarks/client/measure-bundle.sh "$build_dir" "scene-${kit}" \
        --out "${RESULTS_DIR}/client-bundle-${kit}-${HOST_TAG}.json"

    echo "== frame time (${kit}) =="
    node benchmarks/client/frame-time.mjs --dir "$build_dir" --label "scene-${kit}" \
        --out "${RESULTS_DIR}/client-frametime-${kit}-${HOST_TAG}.json"
}

echo "== [2/3] Single-threaded kit (the default WASM client) =="
if [ -x "$QT_WASM_ST/bin/qt-cmake" ]; then
    build_measure_drive "single" "$QT_WASM_ST/bin/qt-cmake"
else
    echo "  single-threaded WASM kit not found at $QT_WASM_ST -- skipping" >&2
fi

echo "== [3/3] Multi-threaded kit (cross-origin isolated, SharedArrayBuffer) =="
if [ -x "$QT_WASM_MT/bin/qt-cmake" ]; then
    build_measure_drive "multi" "$QT_WASM_MT/bin/qt-cmake" -DQT_WASM_PTHREAD_POOL_SIZE=8
else
    echo "  multi-threaded WASM kit not found at $QT_WASM_MT -- skipping" >&2
fi

echo "== done. baselines under ${RESULTS_DIR}/client-*-${HOST_TAG}.json =="
