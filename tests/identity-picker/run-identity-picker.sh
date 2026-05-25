#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The development scope picker, in a real browser, with a real cookie jar:
#  [1] write a three-scope project with the tooling;
#  [2] build its edge with SYNQT_DEV_TOOLS;
#  [3] drive three tabs of one browser context through it with Playwright.
#
# Two tabs share one cookie jar, so only a browser shows they keep two sessions. Three
# static bundles stand in for a client.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

WORK="${IDENTITY_PICKER_WORK:-$REPO_ROOT/build/identity-picker}"
export IDENTITY_PICKER_WORK="$WORK"
rm -rf "$WORK"
mkdir -p "$WORK"

echo "== [1/3] write the project =="
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 tests/identity-picker/make-project.py \
    "$WORK/project" "$REPO_ROOT"

echo
echo "== [2/3] build the edge (host kit, development tools on) =="
# SYNQT_DEV_TOOLS: without it the picker is not in SynQtEdge (tests/dev-exclusion).
cmake -S "$WORK/project" -B "$WORK/native" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DSYNQT_DEV_TOOLS=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$WORK/native"

if [ ! -x "$WORK/native/web" ]; then
    echo "  the edge did not build"
    echo "IDENTITY PICKER: NO-GO"
    exit 1
fi

# Phase 3 needs npm and a browser; without them, say so and skip.
if ! command -v npm >/dev/null 2>&1; then
    echo
    echo "== [3/3] SKIPPED: no npm on this host =="
    echo "IDENTITY PICKER: PARTIAL (the project builds; the browser half was not run)"
    exit 0
fi

echo
echo "== [3/3] three tabs, one cookie jar =="
cd tests/identity-picker/verify
npm install --no-audit --no-fund
npx --yes playwright install chromium firefox ||
    echo "   (a runtime did not install; verify.mjs names the engine it had to skip)"
set +e
node verify.mjs
verdict=$?
set -e
if [ "$verdict" = "3" ]; then
    echo "IDENTITY PICKER: PARTIAL (the project builds; no browser engine would launch)"
    exit 0
fi
exit "$verdict"
