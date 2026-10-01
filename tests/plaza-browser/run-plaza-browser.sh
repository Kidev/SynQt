#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The 3D plaza example, built by `synqt dev` and driven in a real browser:
#  [1] copy examples/plaza, add a telemetry timer, name two people;
#  [2] `synqt dev --identity-picker` builds and serves the edge and the client;
#  [3] two tabs sign in as Alice and Bob and walk (verify/verify.mjs).
# tests/plaza proves the edge's half natively.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"
PORT="${PLAZA_PORT:-8097}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

# Announced, never silent. A skip that prints nothing is indistinguishable from a pass.
if [ ! -x "$QT_WASM/bin/qt-cmake" ] || ! command -v emcc >/dev/null 2>&1; then
    echo "== SKIPPED: no WebAssembly kit at $QT_WASM, or no emcc on PATH =="
    echo "PLAZA BROWSER: SKIPPED (CI runs it in .github/workflows/wasm-proofs.yml)"
    exit 0
fi
# `synqt` finds the WebAssembly kit as a sibling of QTDIR.
export QTDIR="$QT_HOST"

WORK="$REPO_ROOT/build/plaza-browser"
PROJECT="$WORK/plaza"
LOG="$WORK/dev.log"

echo "== [1/3] copy the example and probe the copy =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r examples/plaza "$PROJECT"
rm -rf "$PROJECT/build" "$PROJECT/generated" "$PROJECT/CMakePresets.json" \
    "$PROJECT/CMakeUserPresets.json"
python3 tests/plaza-browser/probe.py "$PROJECT/client/app/Main.qml"
printf -- '- email: alice@example.com\n  scope: user\n- email: bob@example.com\n  scope: user\n' \
    >"$PROJECT/.dev-identities"

echo "== [2/3] synqt dev --identity-picker (builds the edge and the WebAssembly client) =="
PYTHONUNBUFFERED=1 PYTHONPATH="$REPO_ROOT/tools/synqt" \
    python3 -m synqt dev --project-dir "$PROJECT" --identity-picker --no-open --no-watch \
    --port "$PORT" >"$LOG" 2>&1 &
DEV=$!
stop() {
    kill "$DEV" 2>/dev/null || true
    pkill -f "$PROJECT/build/edge/edge" 2>/dev/null || true
    wait "$DEV" 2>/dev/null || true
}
trap stop EXIT

# The first build of a WebAssembly client with Qt Quick 3D takes minutes.
for _ in $(seq 1 1200); do
    if grep -q "serving http://127.0.0.1:$PORT/" "$LOG"; then
        break
    fi
    if ! kill -0 "$DEV" 2>/dev/null; then
        cat "$LOG"
        echo "PLAZA BROWSER: NO-GO (synqt dev stopped before it served)"
        exit 1
    fi
    sleep 2
done
if ! grep -q "serving http://127.0.0.1:$PORT/" "$LOG"; then
    cat "$LOG"
    echo "PLAZA BROWSER: NO-GO (synqt dev never served)"
    exit 1
fi
tail -5 "$LOG"

echo "== [3/3] Alice and Bob in two tabs =="
cd tests/plaza-browser/verify
npm install --no-audit --no-fund
PLAZA_URL="http://127.0.0.1:$PORT" node verify.mjs
