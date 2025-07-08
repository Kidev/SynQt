#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# FIX-2: the multiplayer tutorial's movement authority and guest-list gate, proven end to
# end on the native host kit. The edge runs in one process with the World injected, driven
# by native SynClients acting as browsers.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/fix2-arena -B build/fix2-arena -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/fix2-arena
ctest --test-dir build/fix2-arena --output-on-failure
