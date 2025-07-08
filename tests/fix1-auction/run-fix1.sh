#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# FIX-1: the auction tutorial's hands-on checks, proven end to end on the real gavel system
# (examples/gavel), native host kit. The edge and the mesh run in one process, driven by
# native SynClients acting as browsers.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/fix1-auction -B build/fix1-auction -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/fix1-auction
ctest --test-dir build/fix1-auction --output-on-failure
