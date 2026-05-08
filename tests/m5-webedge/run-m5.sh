#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M5 acceptance. Build the service runtime library and the web-edge test (a throwaway
# localhost TLS cert is generated at configure time), then run it over real TLS.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

# With the development sources, because this suite is where the development scope picker's
# runtime behaviour is proved. That a development edge serves it only when asked. The other
# half of that claim, that a release edge does not contain the picker at all, is
# tests/dev-exclusion's, and it configures both ways to prove it. tests/run-all.sh passes
# the same flag, so a full run and a single-suite run build the same library.
cmake -S tests/m5-webedge -B build/m5-webedge -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_DEV_TOOLS=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m5-webedge

ctest --test-dir build/m5-webedge --output-on-failure
