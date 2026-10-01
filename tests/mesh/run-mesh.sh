#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build the service runtime library and the mesh transport test
# (certificates are generated at configure time), then run it.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/mesh -B build/mesh -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/mesh

# Capture the exit code rather than letting `set -e` abort. On a failure the crash-safe trace
# below is what the run is for, and it must be printed before the script exits non-zero. On Windows
# the test can die before writing any QtTest output at all, so this file is the only record of
# where it got to.
rc=0
ctest --test-dir build/mesh --output-on-failure || rc=$?

trace="build/mesh/mesh-trace.log"
if [ -f "$trace" ]; then
    echo "----- mesh crash-safe trace ($trace) -----"
    cat "$trace"
    echo "----- end mesh crash-safe trace -----"
fi

exit "$rc"
