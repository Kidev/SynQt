#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The stall storefront's edge-delivered pages and page seed, fetched by a native
# SynClient over the authenticated wss link. Native host kit.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/stall -B build/stall -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/stall
ctest --test-dir build/stall --output-on-failure
