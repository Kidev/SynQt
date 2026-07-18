#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M1 acceptance: run the generator's Python unit tests, then build and run the
# in-process QtRO round-trip acceptance test, plus a spot check that malformed
# contracts are rejected with a clear diagnostic.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

echo "== [1/4] synqtc Python unit tests =="
( cd tools/synqtc && python3 -m unittest tests.test_synqtc -v )

echo "== [2/4] malformed contracts are rejected =="
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
printf 'widget Nope {}' > "$tmp/bad.syn"
if ( cd tools/synqtc && python3 -m synqtc "$tmp/bad.syn" --out "$tmp/out" --quiet ) 2>"$tmp/err"; then
    echo "FAIL: malformed contract was accepted"; exit 1
fi
echo "rejected as expected: $(cat "$tmp/err")"

echo "== [3/4] configure + build the acceptance test =="
cmake -S tests/m1-contract -B build/m1-contract -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m1-contract

echo "== [4/4] run the acceptance test =="
ctest --test-dir build/m1-contract --output-on-failure
