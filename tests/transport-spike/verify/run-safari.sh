#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The spike's browser matrix in real Safari.app (Playwright drives WebKit, not Safari). macOS only, with
# a logged-in GUI session. Kept out of run-spike.sh, which runs on Linux in CI.
#
# Once per machine:
#
#     sudo safaridriver --enable
#
# Usage: tests/transport-spike/verify/run-safari.sh
#   SAFARI_WSS=1   also run the wss case (needs the harness cert trusted. See below)

set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "run-safari.sh: Safari.app exists only on macOS; nothing to run here."
    exit 0
fi

case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.12.0/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.12.0/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"
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

echo "== [2/4] Build client (WASM single-threaded) =="
# shellcheck source=../../lib/emsdk.sh
. "$REPO_ROOT/tests/lib/emsdk.sh"
synqt_activate_emsdk
"$QT_WASM/bin/qt-cmake" -S tests/transport-spike -B build/spike-client -G Ninja \
    -DSYNQT_SPIKE_ENTITY=client \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike-client

echo "== [3/4] Self-signed localhost cert for the wss listener =="
# run-spike.sh's throwaway TLS certificate. The wss case needs it trusted:
#
#   sudo security add-trusted-cert -d -r trustRoot \
#       -k /Library/Keychains/System.keychain build/certs/cert.pem
#
# Remove it with `sudo security delete-certificate -c localhost -t
# /Library/Keychains/System.keychain`.
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
