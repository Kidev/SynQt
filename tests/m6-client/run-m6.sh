#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The client runtime and the counter example.
#  [1] native functional test (the runtime = the desktop runtime): two clients sync,
#      state transitions, reconnect, route guard;
#  [2] build the desktop counter app and the WASM counter bundle from one QML;
#  [3] browser end-to-end (the WASM client against the real edge) via Playwright, in
#      every engine whose runtime is installed.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

echo "== [1/3] native functional test =="
cmake -S tests/m6-client -B build/m6-client -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m6-client
ctest --test-dir build/m6-client --output-on-failure

# Phases 2 and 3 need the WebAssembly kit and a browser; without them, say so and skip.
if [ ! -x "$QT_WASM/bin/qt-cmake" ]; then
    echo
    echo "== [2/3] and [3/3] SKIPPED: no WebAssembly kit at $QT_WASM =="
    echo "   (set QT_WASM to a wasm kit with QtRemoteObjects built in to run the browser half;"
    echo "    CI runs it in .github/workflows/wasm-proofs.yml, which installs that kit)"
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

# SynQt's loading page, as `synqt build` renders it (tools/wasm-shell.py); no inline script.
python3 tools/wasm-shell.py --target counter-client --out build/m6-app-wasm

echo "== [3/3] browser end-to-end =="
cd tests/m6-client/verify
npm install --no-audit --no-fund
# Every engine the harness can drive; verify.mjs reports any it skips (docs/browser-proofs.md).
npx --yes playwright install chromium firefox webkit ||
    echo "   (a runtime did not install; verify.mjs names the engine it had to skip)"
node verify.mjs
