#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The multiplayer tutorial's movement authority and guest-list gate, proven end to
# end on the native host kit. The edge runs in one process with the World injected, driven
# by native SynClients acting as browsers.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/arena -B build/arena -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/arena
ctest --test-dir build/arena --output-on-failure
