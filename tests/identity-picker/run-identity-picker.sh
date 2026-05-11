#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The development scope picker, in a real browser, with a real cookie jar.
#  [1] write a three-scope project with the tooling;
#  [2] build its edge with SYNQT_DEV_TOOLS, which is the only build that has a picker;
#  [3] drive three tabs of one browser context through it with Playwright.
#
# The claim under test cannot be made from inside a process. Two tabs of one browser share
# one cookie jar (RFC 6265 scopes a cookie to a host, not a port), and "the second sign-in
# did not become the first" is a statement about that jar. tests/m5-webedge proves the edge
# sets two cookies. Only a browser proves two tabs then keep two sessions.
#
# No WebAssembly kit is needed and no client is built, which bundle the edge answers with
# is how this suite asks a tab who it is, and three directories of static HTML answer that
# question exactly as a compiled client would.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
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
# SYNQT_DEV_TOOLS is the compile-time half of the picker's three layers: without it the
# class is not in SynQtEdge at all, and --identity-picker has nothing to switch on. This is
# what `synqt dev` configures and what `synqt build` never does (tests/dev-exclusion).
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

# Phase 3 needs npm and a browser. Phases 1 and 2 need neither. Where they are absent, say
# so plainly rather than fail the suite on a host that was never given them. Announced,
# never silent. A skip that prints nothing is indistinguishable from a pass.
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
