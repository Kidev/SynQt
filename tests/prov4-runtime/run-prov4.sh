#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# PROV-4: EntityRuntime injects a blueprint's backend helper (Db/Cache/Http/Jobs) into every
# owned Source from the entity's provider config, with no manual wiring. Native host kit.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/prov4-runtime -B build/prov4-runtime -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/prov4-runtime
ctest --test-dir build/prov4-runtime --output-on-failure
