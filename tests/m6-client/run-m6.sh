#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M6: the client runtime and the counter example.
#  [1] native functional test (the runtime = the desktop runtime): two clients sync,
#      state transitions, reconnect, route guard;
#  [2] build the desktop counter app and the WASM counter bundle from one QML;
#  [3] browser end-to-end (the WASM client against the real edge) via Playwright.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.11.1/wasm_singlethread}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

echo "== [1/3] native functional test =="
cmake -S tests/m6-client -B build/m6-client -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m6-client
ctest --test-dir build/m6-client --output-on-failure

# Phases 2 and 3 need the WebAssembly kit and a browser; phase 1 needs neither. Where the kit is
# absent, run what can run and say plainly what did not, rather than fail the whole suite on a
# toolchain this host was never given (the native client runtime IS the desktop runtime, so
# phase 1 is a real result on its own). Announced, never silent: a skip that prints nothing is
# indistinguishable from a pass.
if [ ! -x "$QT_WASM/bin/qt-cmake" ]; then
    echo
    echo "== [2/3] and [3/3] SKIPPED: no WebAssembly kit at $QT_WASM =="
    echo "   (set QT_WASM to a wasm kit with QtRemoteObjects built in to run the browser half;"
    echo "    the WASM/browser paths are covered by .github/workflows/browser-matrix.yml)"
    echo
    echo "M6 GATE: PARTIAL (native runtime only; WASM bundle and browser e2e not exercised)"
    exit 0
fi

echo "== [2/3] build desktop app + edge, and the WASM client =="
cmake -S tests/m6-client/app -B build/m6-app-desktop -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m6-app-desktop   # counter-client (desktop) + counter-edge
"$QT_WASM/bin/qt-cmake" -S tests/m6-client/app -B build/m6-app-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m6-app-wasm       # counter-client.wasm

# The served page is SynQt's loading page, rendered by the same code `synqt build` uses
# for a real app (tools/wasm-shell.py). Qt's generated template is doubly unfit here: it
# is Qt-branded on a white background, and it boots from an inline onload handler that no
# CSP hash can allow. This page has no inline script at all.
python3 tools/wasm-shell.py --target counter-client --out build/m6-app-wasm

echo "== [3/3] browser end-to-end =="
cd tests/m6-client/verify
npm install --no-audit --no-fund
npx --yes playwright install chromium
node verify.mjs
