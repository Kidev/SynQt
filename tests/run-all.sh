#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Configure and build the framework and every suite in one tree, run them under one ctest,
# then run the suites whose entry point is a generator (listed by tests/CMakeLists.txt).
#
#   QT_HOST=/path/to/qt/gcc_64 tests/run-all.sh
#
# BUILD_DIR moves the tree (default build/all); SYNQT_PHASES runs one phase.

set -euo pipefail

# Which phases to run. Anything but these values is refused, never defaulted.
SYNQT_PHASES="${SYNQT_PHASES:-all}"
case "$SYNQT_PHASES" in
all | tree | generated) ;;
*)
    echo "error: SYNQT_PHASES must be all, tree, or generated (got '$SYNQT_PHASES')" >&2
    exit 2
    ;;
esac

# No default: the kit directory name differs per platform.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.12.0/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.12.0/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
BUILD_DIR="${BUILD_DIR:-build/all}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=tests/lib/compiler-cache.sh
. "$REPO_ROOT/tests/lib/compiler-cache.sh"

# QtTest logs to the debugger on Windows when stdout is a pipe; this sends it to stderr
# (qtbase/src/corelib/global/qlogging.cpp). A no-op elsewhere.
export QT_FORCE_STDERR_LOGGING=1

mkdir -p "$BUILD_DIR"
log="$BUILD_DIR/configure-build.log"

if [ "$SYNQT_PHASES" = "generated" ]; then
    echo "== [1/3] configure the tree (no build: see below) =="
else
    echo "== [1/3] configure and build the tree =="
fi
# pipefail: the status is cmake's, not tee's.
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSYNQT_DEV_TOOLS=ON 2>&1 | tee "$log"
# Configure in every phase: it writes $BUILD_DIR/script-suites.txt. Only phase [2/3]
# builds.
if [ "$SYNQT_PHASES" != "generated" ]; then
    cmake --build "$BUILD_DIR" 2>&1 | tee -a "$log"
fi

# Any CMake warning fails the run, except Qt 6.12.0's own Qt6Graphs block: its targets
# file requires Qt6::Graphs2DImpl, which its dependencies file never provides, and a static
# build reaches it through plugin scanning. It is skipped by name, so it fails again once
# Qt fixes it.
cmake_warnings() {
    awk '
        /^CMake Warning/ { if (in_block) emit(); in_block = 1; block = $0; upstream = 0; next }
        in_block {
            if ($0 ~ /^(-- |\[|CMake )/) { emit(); next }
            block = block "\n" $0
            if ($0 ~ /Qt6Graphs/) upstream = 1
        }
        END { if (in_block) emit() }
        function emit() { if (!upstream) print block; in_block = 0 }
    ' "$1"
}

warnings="$(cmake_warnings "$log")"
if [ -n "$warnings" ]; then
    echo "error: the tree configured with CMake warnings (see $log)" >&2
    echo "$warnings" >&2
    exit 1
fi

# Phase [2/3], not indented: the quoted heredoc needs its terminator at column zero.
if [ "$SYNQT_PHASES" != "generated" ]; then

echo
echo "== [2/3] run the suites =="
# Serial: several suites bind fixed ports.
if ! ctest --test-dir "$BUILD_DIR" --output-on-failure; then
    # ctest never prints a failing test's exit code, which is often the whole diagnosis on
    # Windows. Rerun each failing test, with a deadline, to print it.
    echo
    echo "----- exit code of each failing test (ctest reports only their output) -----"
    ctest --test-dir "$BUILD_DIR" --rerun-failed --show-only=json-v1 \
        > "$BUILD_DIR/failed-tests.json" || true
    python3 - "$BUILD_DIR/failed-tests.json" <<'PY' || true
import json
import subprocess
import sys

listing = json.load(open(sys.argv[1], encoding="utf-8"))
for test in listing.get("tests", []):
    command = test.get("command")
    if not command:
        continue
    directory = None
    for prop in test.get("properties", []):
        if prop.get("name") == "WORKING_DIRECTORY":
            directory = prop.get("value") or None
    try:
        code = subprocess.run(command, cwd=directory, timeout=300).returncode
    except subprocess.TimeoutExpired:
        print("%s: no exit within 300s" % test["name"], flush=True)
        continue
    except OSError as error:
        print("%s: could not be started: %s" % (test["name"], error), flush=True)
        continue
    print("%s: exit code %d (0x%x)" % (test["name"], code, code & 0xFFFFFFFF), flush=True)
PY
    echo "----- end of exit codes -----"
    exit 1
fi

# m3's crash-safe trace, when there is one.
trace="$BUILD_DIR/tests/m3-mesh/m3-trace.log"
if [ -f "$trace" ]; then
    echo "----- m3 crash-safe trace ($trace) -----"
    cat "$trace"
    echo "----- end m3 crash-safe trace -----"
fi

fi # SYNQT_PHASES != generated

if [ "$SYNQT_PHASES" = "tree" ]; then
    echo
    echo "== [3/3] skipped (SYNQT_PHASES=tree) =="
    exit 0
fi

echo
echo "== [3/3] the suites that compile generated output =="
fail=0
failed=""
warned=""
while read -r suite; do
    # Strip the \r that CMake's text-mode file(WRITE) adds on Windows.
    suite="${suite%$'\r'}"
    [ -n "$suite" ] || continue
    runner="$(echo tests/"$suite"/run-*.sh)"
    suite_log="$BUILD_DIR/$suite.log"
    echo "-- $runner"
    # Tee each tree's configure so the warning gate sees it.
    if bash "$runner" 2>&1 | tee "$suite_log"; then
        echo "PASS $runner"
    else
        echo "FAIL $runner"
        fail=1
        failed="$failed $runner"
    fi
    if [ -n "$(cmake_warnings "$suite_log")" ]; then
        warned="$warned $runner"
    fi
done < "$BUILD_DIR/script-suites.txt"

if [ -n "$warned" ]; then
    echo "error: suites that configured with CMake warnings:$warned" >&2
    fail=1
fi
if [ -n "$failed" ]; then
    echo "error: failing suites:$failed" >&2
fi
exit "$fail"
