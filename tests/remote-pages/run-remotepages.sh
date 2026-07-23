#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Remote pages acceptance: build and run the page-store and client-loader suites.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/remote-pages -B build/remote-pages -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/remote-pages

ctest --test-dir build/remote-pages --output-on-failure
