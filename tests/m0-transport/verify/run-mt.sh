#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Multi-threaded WASM proof: builds the M0 client with the wasm_multithread kit and the
# native edge, then drives headless Chromium to assert the threaded client only gets
# SharedArrayBuffer under cross-origin isolation (COOP: same-origin + COEP: require-corp,
# the headers the M5 edge emits), while the same bundle served without those headers is
# not isolated. This is the CLIENT-2 runtime check the single-threaded run-m0.sh does not
# cover. Reuses the M0 sources, so nothing new to maintain but the harness.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
QT_WASM_MT="${QT_WASM_MT:-/opt/Qt/6.11.1/wasm_multithread}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SPIKE="$REPO_ROOT/tests/m0-transport"
cd "$REPO_ROOT"

echo "== [1/4] Build edge (native host kit) =="
cmake -S tests/m0-transport -B build/m0-edge -G Ninja \
    -DSYNQT_M0_ENTITY=edge \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m0-edge

echo "== [2/4] Build client (WASM multi-threaded) =="
"$QT_WASM_MT/bin/qt-cmake" -S tests/m0-transport -B build/m0-client-mt -G Ninja \
    -DSYNQT_M0_ENTITY=client \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-client-mt

echo "== [3/4] Install Playwright + Chromium =="
cd "$SPIKE/verify"
npm install --no-audit --no-fund
npx --yes playwright install chromium

echo "== [4/4] Run the cross-origin-isolation + threaded-QtRO proof =="
MT_HEADLESS=1 node verify-mt.mjs
