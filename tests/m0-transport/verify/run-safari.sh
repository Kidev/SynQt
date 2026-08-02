#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The M0 matrix in real Safari.app, which no other harness reaches: verify.mjs drives
# Playwright's WebKit, and that is Safari's engine rather than Safari (see the note in
# verify-safari.mjs). macOS only, and needs a logged-in GUI session, because Safari has no
# headless mode.
#
# Kept out of run-m0.sh deliberately. That script is the M0 gate and runs in CI on Linux, where
# Safari does not exist; folding a macOS-and-display-only case into it would make the gate's
# result depend on which machine ran it.
#
# One manual step, once per machine:
#
#     sudo safaridriver --enable
#
# Usage: tests/m0-transport/verify/run-safari.sh
#   SAFARI_WSS=1   also run the wss case (needs the harness cert trusted; see below)

set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "run-safari.sh: Safari.app exists only on macOS; nothing to run here."
    exit 0
fi

case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.11.1/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.11.1/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
QT_WASM="${QT_WASM:-/opt/Qt/6.11.1/wasm_singlethread}"
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

echo "== [2/4] Build client (WASM single-threaded) =="
"$QT_WASM/bin/qt-cmake" -S tests/m0-transport -B build/m0-client -G Ninja \
    -DSYNQT_M0_ENTITY=client \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/m0-client

echo "== [3/4] Self-signed localhost cert for the wss listener =="
# The same throwaway public-link TLS server cert run-m0.sh mints; not a mesh CA, and nothing
# under synqt/mesh/ is created. Safari will not connect over wss until this is trusted in the
# system keychain, which is why the wss case is opt-in:
#
#   sudo security add-trusted-cert -d -r trustRoot \
#       -k /Library/Keychains/System.keychain build/certs/cert.pem
#
# and then re-run with SAFARI_WSS=1. Removing it again is `sudo security delete-certificate
# -c localhost -t /Library/Keychains/System.keychain`.
mkdir -p build/certs
if [ ! -f build/certs/cert.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 \
        -keyout build/certs/key.pem -out build/certs/cert.pem \
        -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
fi

echo "== [4/4] Drive Safari.app through safaridriver =="
cd "$SPIKE/verify"
npm install --no-audit --no-fund
node verify-safari.mjs
