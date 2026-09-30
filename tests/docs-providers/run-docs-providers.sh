#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The custom providers the advanced tutorials build, compiled from the pages and linked
# against the real provider library. Native host kit.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/docs-providers -B build/docs-providers -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/docs-providers
ctest --test-dir build/docs-providers --output-on-failure
