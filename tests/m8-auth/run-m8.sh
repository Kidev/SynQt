#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M8: the edge login flow (native, against the in-process dev stub provider) and the
# `synqt add auth` scaffolding. jwt-cpp + picojson (vcpkg) are required for ID-token
# verification; the include dir is auto-detected or set with -DJWT_CPP_INCLUDE_DIR=.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

echo "== [1/2] edge OAuth/OIDC login (native, dev stub provider) =="
cmake -S tests/m8-auth -B build/m8-auth -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m8-auth
ctest --test-dir build/m8-auth --output-on-failure

echo "== [2/2] synqt add auth scaffolding =="
# The auth scaffolding only, which is what this suite is about. This used to be a bare
# `unittest discover`, which ran every Python test in the project: already covered by
# tests.yml on three operating systems, and the build tests in there compile a real
# WebAssembly client, so it added minutes to a C++ suite for no coverage.
cd tools/synqt
PYTHONPATH=. python3 -m unittest tests.test_addauth -v
