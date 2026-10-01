#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Multi-threaded WASM proof: the threaded spike client gets SharedArrayBuffer only under COOP
# same-origin and COEP require-corp, as the edge emits them.

set -euo pipefail

# No default: the kit directory name differs per platform.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.12.0/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.12.0/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
QT_WASM_MT="${QT_WASM_MT:-/opt/Qt/6.12.0/wasm_multithread}"

# The host tools for the cross build, defaulting to the host kit.
export QT_HOST_PATH="${QT_HOST_PATH:-$QT_HOST}"

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SPIKE="$REPO_ROOT/tests/transport-spike"
cd "$REPO_ROOT"

echo "== [1/4] Build edge (native host kit) =="
cmake -S tests/transport-spike -B build/spike-edge -G Ninja \
    -DSYNQT_SPIKE_ENTITY=edge \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/spike-edge

echo "== [2/4] Build client (WASM multi-threaded) =="
# shellcheck source=../../lib/emsdk.sh
. "$REPO_ROOT/tests/lib/emsdk.sh"
synqt_activate_emsdk
"$QT_WASM_MT/bin/qt-cmake" -S tests/transport-spike -B build/spike-client-mt -G Ninja \
    -DSYNQT_SPIKE_ENTITY=client \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike-client-mt

echo "== [3/4] Install Playwright + the browser engines =="
cd "$SPIKE/verify"
npm install --no-audit --no-fund
# All three engines; verify-mt.mjs names any it skips.
npx --yes playwright install chromium firefox webkit ||
    echo "   (a runtime did not install; verify-mt.mjs names the engine it had to skip)"

echo "== [4/4] Run the cross-origin-isolation + threaded-QtRO proof =="
MT_HEADLESS=1 node verify-mt.mjs
