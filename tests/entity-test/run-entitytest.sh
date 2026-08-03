#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The QML test harness (`import SynQt.Test`), driven against a Source written the way an
# application writes one. This is the framework's copy of what `synqt test` generates for
# a project, so it breaks here before it breaks in someone's app.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/entity-test -B build/entity-test -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/entity-test

ctest --test-dir build/entity-test --output-on-failure
