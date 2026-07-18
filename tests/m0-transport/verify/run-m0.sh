#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# One-shot M0 go/no-go: builds the edge (native host kit) and the client (WASM
# single-threaded), mints a throwaway self-signed localhost cert for the wss
# listener, installs Playwright, and runs the browser matrix + reconnect test.
# This is NOT a mesh CA: it is a public-link TLS server cert, and nothing under
# synqt/mesh/ is created.

set -euo pipefail

# The pinned kits. Overridable via env so CI (which provisions Qt to its own outdir) can point
# these at the runner's install without editing the script.
QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.11.1/wasm_singlethread}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SPIKE="$REPO_ROOT/tests/m0-transport"
cd "$REPO_ROOT"

echo "== [1/5] Build edge (native host kit) =="
cmake -S tests/m0-transport -B build/m0-edge -G Ninja \
    -DSYNQT_M0_ENTITY=edge \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m0-edge

echo "== [2/5] Build client (WASM single-threaded) =="
"$QT_WASM/bin/qt-cmake" -S tests/m0-transport -B build/m0-client -G Ninja \
    -DSYNQT_M0_ENTITY=client \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-client

echo "== [3/5] Self-signed localhost cert for the wss listener =="
mkdir -p build/certs
if [ ! -f build/certs/cert.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout build/certs/key.pem -out build/certs/cert.pem \
        -days 5 -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
fi

echo "== [4/5] Install Playwright + browser engines =="
cd "$SPIKE/verify"
npm install --no-audit --no-fund
npx --yes playwright install chromium firefox

echo "== [5/5] Run the M0 browser matrix + reconnect =="
node verify.mjs
