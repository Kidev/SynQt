#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M0 go/no-go: build the edge and the single-threaded client, mint a throwaway localhost
# TLS certificate for wss, and run the browser matrix and the reconnect test.

set -euo pipefail

# The pinned kits, overridable from the environment. No default host kit directory.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.12.0/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.12.0/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"

# The host tools for the cross build.
export QT_HOST_PATH="${QT_HOST_PATH:-$QT_HOST}"
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
# shellcheck source=../../lib/emsdk.sh
. "$REPO_ROOT/tests/lib/emsdk.sh"
synqt_activate_emsdk
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

echo "== [5/5] Run the transport spike's browser matrix + reconnect =="
node verify.mjs
