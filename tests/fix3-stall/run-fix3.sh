#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# FIX-3: the stall storefront's edge-delivered pages and its real page seed, proven end to
# end on the native host kit. The edge runs in one process serving its Pages connect point;
# a native SynClient acts as the browser, fetching pages over the same authenticated wss
# link. The seed is driven through the production per-connection Caller, not a hand-built
# one.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/fix3-stall -B build/fix3-stall -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/fix3-stall
ctest --test-dir build/fix3-stall --output-on-failure
