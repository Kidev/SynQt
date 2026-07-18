#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M2 acceptance: build the client runtime library and the transport acceptance test,
# then run it (acquire a host Source through WebSocketTransport over a real local
# plaintext WebSocket; property change reaches the Replica; slot call reaches the
# Source).

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/m2-transport -B build/m2-transport -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m2-transport

ctest --test-dir build/m2-transport --output-on-failure
