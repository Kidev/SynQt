#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# SessionManager, the Caller accessor, connect-point scope gating, and per-peer
# authorization, proven on the three-entity todo (native host kit. The mesh and the edge
# run in one process, driven by native SynClients acting as browsers).

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/caller -B build/caller -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/caller
ctest --test-dir build/caller --output-on-failure
