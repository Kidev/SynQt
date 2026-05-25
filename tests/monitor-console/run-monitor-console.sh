#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The monitoring console, end to end, in a real browser:
#  [1] write the project with the scaffolder;
#  [2] build the monitor and the reporting edge with the host kit;
#  [3] build the console with the WebAssembly kit;
#  [4] drive the delivery gate, the operator sign-in and the console via Playwright.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

WORK="${MONITOR_CONSOLE_WORK:-$REPO_ROOT/build/monitor-console}"
export MONITOR_CONSOLE_WORK="$WORK"
rm -rf "$WORK"
mkdir -p "$WORK"

echo "== [1/4] write the project with the scaffolder =="
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 tests/monitor-console/make-project.py \
    "$WORK/project" "$REPO_ROOT"

echo
echo "== [2/4] build the monitor and the reporting edge (host kit) =="
cmake -S "$WORK/project" -B "$WORK/native" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$WORK/native"

for entity in ops web; do
    if [ ! -x "$WORK/native/$entity" ]; then
        echo "  $entity did not build"
        echo "MONITOR CONSOLE: NO-GO"
        exit 1
    fi
done

# Phases 3 and 4 need the WebAssembly kit and a browser; without them, say so and skip.
if [ ! -x "$QT_WASM/bin/qt-cmake" ]; then
    echo
    echo "== [3/4] and [4/4] SKIPPED: no WebAssembly kit at $QT_WASM =="
    echo "   (set QT_WASM to a wasm kit with QtRemoteObjects built in to run the browser"
    echo "    half; CI runs it in .github/workflows/wasm-proofs.yml)"
    echo
    echo "MONITOR CONSOLE: PARTIAL (the project scaffolds and builds; the console was not run)"
    exit 0
fi

echo
echo "== [3/4] build the console for the browser (WebAssembly kit) =="
"$QT_WASM/bin/qt-cmake" -S "$WORK/project" -B "$WORK/wasm" -G Ninja \
    -DSYNQT_ROOT="$REPO_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$WORK/wasm"

# The operator scope's bundle holds only the console's files.
mkdir -p "$WORK/bundle-console"
cp "$WORK/wasm/ops-console.js" "$WORK/wasm/ops-console.wasm" "$WORK/wasm/qtloader.js" \
    "$WORK/bundle-console/"
# The page the browser loads first, rendered by the same code `synqt build` uses. It has no
# inline script the CSP cannot allow. Its hashes are collected from the bundle at startup.
python3 tools/wasm-shell.py --target ops-console --out "$WORK/bundle-console"

echo
echo "== [4/4] browser end to end =="
cd tests/monitor-console/verify
npm install --no-audit --no-fund
npx --yes playwright install chromium firefox webkit ||
    echo "   (a runtime did not install; verify.mjs names the engine it had to skip)"
node verify.mjs
