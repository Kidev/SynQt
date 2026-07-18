#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M3 acceptance: build the service runtime library and the mesh transport test
# (certificates are generated at configure time), then run it.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/m3-mesh -B build/m3-mesh -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m3-mesh

# Capture the exit code rather than letting `set -e` abort: on a failure the crash-safe trace
# below is the whole point, and it must be printed before the script exits non-zero. On Windows
# the test can die before writing any QtTest output at all, so this file is the only record of
# where it got to.
rc=0
ctest --test-dir build/m3-mesh --output-on-failure || rc=$?

trace="build/m3-mesh/m3-trace.log"
if [ -f "$trace" ]; then
    echo "----- m3 crash-safe trace ($trace) -----"
    cat "$trace"
    echo "----- end m3 crash-safe trace -----"
fi

exit "$rc"
