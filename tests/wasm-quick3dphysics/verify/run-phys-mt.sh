#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The Qt Quick 3D Physics scene on the multi-threaded WebAssembly kit, served with COOP
# same-origin and COEP require-corp, as the edge emits under
# security.cross_origin_isolation.
#
#   (default)   build, then check headless that the page is isolated and boots.
#   --serve     build and keep serving, to watch the box fall in a real browser.
#
# Needs /opt/Qt/6.12.0/wasm_multithread, emsdk 5.0.5 and Node. Usage:
#   tests/wasm-quick3dphysics/verify/run-phys-mt.sh
#   tests/wasm-quick3dphysics/verify/run-phys-mt.sh --serve

set -euo pipefail

QT_WASM_MT="${QT_WASM_MT:-/opt/Qt/6.12.0/wasm_multithread}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HERE="$REPO_ROOT/tests/wasm-quick3dphysics"
cd "$REPO_ROOT"

if [ ! -x "$QT_WASM_MT/bin/qt-cmake" ]; then
    echo "error: multi-threaded WASM kit not found at $QT_WASM_MT" >&2
    echo "       install it (aqtinstall wasm_multithread for 6.12.0) and retry." >&2
    exit 1
fi

MODE="check"
if [ "${1:-}" = "--serve" ]; then
    MODE="serve"
fi

echo "== [1/3] Build the scene (WASM multi-threaded: Quick3D + bundled PhysX + pthreads) =="
# A preallocated worker pool: browsers disallow creating threads from the main thread.
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
