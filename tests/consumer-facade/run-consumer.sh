#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The consumer facade: `<Contract>.on<Signal>` attached handlers and the returning-slot
# `.then(...)` promise, plus facade forwarding of a property, model, void slot and signal.
# In-process over the real WebSocketTransport, consumed from real QML. Native host kit.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/consumer-facade -B build/consumer-facade -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/consumer-facade

ctest --test-dir build/consumer-facade --output-on-failure
