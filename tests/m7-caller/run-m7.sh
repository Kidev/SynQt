#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M7: SessionManager, the Caller accessor, connect-point scope gating, and per-peer
# authorization, proven on the three-entity todo (native host kit; the mesh and the edge
# run in one process, driven by native SynClients acting as browsers).

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/m7-caller -B build/m7-caller -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m7-caller
ctest --test-dir build/m7-caller --output-on-failure
