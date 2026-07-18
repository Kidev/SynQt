#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Try the Qt Quick 3D Physics scene on the MULTI-THREADED WebAssembly kit.
#
# The single-threaded build (run-phys.sh) loads and boots but its box never falls in a
# browser. The working hypothesis is that Qt Quick 3D Physics steps PhysX on a worker thread
# the single-threaded kit cannot spawn. This script builds the identical scene with the
# wasm_multithread kit and serves it under the cross-origin-isolation headers SharedArrayBuffer
# needs (COOP: same-origin, COEP: require-corp); exactly what the SynQt web edge emits when
# security.cross_origin_isolation is on; so pthreads, and the PhysX worker, can actually run.
#
# Two modes:
#   (default)   build + a headless check that the page is cross-origin isolated and boots, and
#               whether the box advances under headless automation.
#   --serve     build + keep serving with the COI headers so you can open the printed URL in a
#               REAL browser tab and watch the box fall. This is the definitive interactive
#               check the headless rAF loop cannot make for you.
#
# Needs the multi-threaded WASM kit (/opt/Qt/6.11.1/wasm_multithread) with emsdk 4.0.7, and
# Node for the driver. Usage:
#   tests/wasm-quick3dphysics/verify/run-phys-mt.sh            # build + headless check
#   tests/wasm-quick3dphysics/verify/run-phys-mt.sh --serve    # build + interactive serve

set -euo pipefail

QT_WASM_MT="${QT_WASM_MT:-/opt/Qt/6.11.1/wasm_multithread}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HERE="$REPO_ROOT/tests/wasm-quick3dphysics"
cd "$REPO_ROOT"

if [ ! -x "$QT_WASM_MT/bin/qt-cmake" ]; then
    echo "error: multi-threaded WASM kit not found at $QT_WASM_MT" >&2
    echo "       install it (aqtinstall wasm_multithread for 6.11.1) and retry." >&2
    exit 1
fi

MODE="check"
if [ "${1:-}" = "--serve" ]; then
    MODE="serve"
fi

echo "== [1/3] Build the scene (WASM multi-threaded: Quick3D + bundled PhysX + pthreads) =="
# QT_WASM_PTHREAD_POOL_SIZE preallocates the worker pool the PhysX task dispatcher draws from;
# without a pool the runtime would have to grow threads on demand, which browsers disallow
# from the main thread. 8 is comfortably above what one PhysicsWorld needs.
"$QT_WASM_MT/bin/qt-cmake" -S tests/wasm-quick3dphysics -B build/q3dphys-wasm-mt -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQT_WASM_PTHREAD_POOL_SIZE=8
cmake --build build/q3dphys-wasm-mt

echo "== [2/3] Install the browser driver =="
cd "$HERE/verify"
npm install --no-audit --no-fund
npx --yes playwright install chromium

if [ "$MODE" = "serve" ]; then
    echo "== [3/3] Serve the multi-threaded build with COOP/COEP (interactive) =="
    exec env PHYS_SERVE=1 node verify-phys-mt.mjs
fi

echo "== [3/3] Headless: confirm cross-origin isolation + boot, sample the fall =="
# Exit 0 = isolated+booted+box fell; 3 = isolated+booted but frame loop idle under headless
# (open the --serve URL in a real tab to see it fall); 1/2 = failure.
set +e
PHYS_HEADLESS=1 node verify-phys-mt.mjs
rc=$?
set -e
if [ "$rc" = "3" ]; then
    echo
    echo "Headless could not drive the render loop past boot; expected under automation."
    echo "Run  tests/wasm-quick3dphysics/verify/run-phys-mt.sh --serve  and open the URL in a"
    echo "real browser tab to confirm the box falls on the multi-threaded kit."
fi
exit "$rc"
