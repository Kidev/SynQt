#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The edge login flow (native, against the in-process dev stub provider) and the
# `synqt add auth` scaffolding. jwt-cpp + picojson (vcpkg) are required for ID-token
# verification. The include dir is auto-detected or set with -DJWT_CPP_INCLUDE_DIR=.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

echo "== [1/2] edge OAuth/OIDC login (native, dev stub provider) =="
# -DSYNQT_DEV_TOOLS=ON: this tree compiles the development-only sources, because the
# suites that test them construct them directly. A shipped build never does, and
# tests/dev-exclusion is what proves the difference is real.
cmake -S tests/m8-auth -B build/m8-auth -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSYNQT_DEV_TOOLS=ON
cmake --build build/m8-auth
ctest --test-dir build/m8-auth --output-on-failure

echo "== [2/2] synqt add auth scaffolding =="
# The auth scaffolding only, which is what this suite is about. A bare
# `unittest discover` would run every Python test in the project, which tests.yml already
# covers on three operating systems, and the build tests in there compile a real
# WebAssembly client, so it would add minutes to a C++ suite for no coverage.
cd tools/synqt
PYTHONPATH=. python3 -m unittest tests.test_addauth -v
