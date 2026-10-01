#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build the service runtime library and the web-edge test (a throwaway
# localhost TLS cert is generated at configure time), then run it over real TLS.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

# With the development sources: this suite proves the picker's runtime behaviour.
cmake -S tests/webedge -B build/webedge -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_DEV_TOOLS=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/webedge

ctest --test-dir build/webedge --output-on-failure
