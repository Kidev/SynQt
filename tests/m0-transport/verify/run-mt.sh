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

# The host kit builds the edge and provides the cross build's host tools. Its directory name
# is the host's, not the target's, so defaulting to the Linux one makes this script fail on
# macOS with a CMake error about a prefix that was never going to exist there.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.11.1/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.11.1/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
QT_WASM_MT="${QT_WASM_MT:-/opt/Qt/6.11.1/wasm_multithread}"

# A cross-compiled Qt cannot find its own host tools: the WASM kit is host-independent and
# carries the path from Qt's own build machine. CI passes this in, so only a developer
# running the script by hand meets the failure, and it reads as a missing Qt6 package rather
# than as an unset variable. Default it to the host kit this script already resolved.
export QT_HOST_PATH="${QT_HOST_PATH:-$QT_HOST}"

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
# shellcheck source=../../lib/emsdk.sh
. "$REPO_ROOT/tests/lib/emsdk.sh"
synqt_activate_emsdk
"$QT_WASM_MT/bin/qt-cmake" -S tests/m0-transport -B build/m0-client-mt -G Ninja \
    -DSYNQT_M0_ENTITY=client \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-client-mt

echo "== [3/4] Install Playwright + the browser engines =="
cd "$SPIKE/verify"
npm install --no-audit --no-fund
# All three, because cross-origin isolation and SharedArrayBuffer are engine decisions and
# a claim proven in one engine is a claim about that engine. A runtime that will not
# install is not fatal: verify-mt.mjs probes by launching and names the engine it skipped.
npx --yes playwright install chromium firefox webkit ||
    echo "   (a runtime did not install; verify-mt.mjs names the engine it had to skip)"

echo "== [4/4] Run the cross-origin-isolation + threaded-QtRO proof =="
MT_HEADLESS=1 node verify-mt.mjs
