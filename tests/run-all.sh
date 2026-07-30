#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The whole host-kit test story in one command: configure and build the framework and every
# suite in one tree, run them under one ctest, then run the suites whose entry point is a
# generator rather than CMake (tests/CMakeLists.txt publishes that list; this script does
# not keep its own copy of it).
#
# The per-suite run-*.sh scripts still work and are still the right tool for one milestone
# at a time. This is the whole thing, and it is what CI runs.
#
#   QT_HOST=/path/to/qt/gcc_64 tests/run-all.sh
#
# BUILD_DIR overrides where the tree lands (default build/all).

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
BUILD_DIR="${BUILD_DIR:-build/all}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

mkdir -p "$BUILD_DIR"
log="$BUILD_DIR/configure-build.log"

echo "== [1/3] configure and build the tree =="
# pipefail is set, so the exit status is cmake's and not tee's; without it a failing
# configure would look like a pass.
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tee "$log"
cmake --build "$BUILD_DIR" 2>&1 | tee -a "$log"

# A CMake warning is a defect, not decoration. The two this gate was built for were a real
# incomplete-linking report (a SynQt library publicly links a Qt module the consumer's scope
# could not resolve) and a Qt module missing from the kit, and both scrolled past in green
# builds for as long as the workflow existed.
if grep -q "CMake Warning" "$log"; then
    echo "error: the tree configured with CMake warnings (see $log)" >&2
    grep -n -A3 "CMake Warning" "$log" >&2
    exit 1
fi

echo
echo "== [2/3] run the suites =="
# Serial on purpose: several suites bind fixed ports and start real servers, so -j needs an
# audit of what they listen on, not just a flag.
if ! ctest --test-dir "$BUILD_DIR" --output-on-failure; then
    # ctest prints a failing test's captured output and nothing else, so a test that exits
    # non-zero having written nothing reports as a blank line. That is exactly what the
    # Windows column produced for tst_envfile and tst_pagestore, and it is undiagnosable:
    # a QTest binary flushes its log line by line, so an empty log means the process never
    # reached its first assertion, which a loader failure and an early exit both explain.
    # The exit code separates them (0xc0000135 is a missing DLL; a small number is QTest's
    # count of failed test functions) and ctest never prints it, so run each failing test
    # again for it. Only failing tests are re-run, and each is given a deadline, so a test
    # that failed by hanging cannot hang this too.
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

# m3 keeps a crash-safe trace because on Windows a piped standard stream is block-buffered
# and a hard exit loses the whole ctest log. It is the only record of where the process got
# to, so print it when there is one.
trace="$BUILD_DIR/tests/m3-mesh/m3-trace.log"
if [ -f "$trace" ]; then
    echo "----- m3 crash-safe trace ($trace) -----"
    cat "$trace"
    echo "----- end m3 crash-safe trace -----"
fi

echo
echo "== [3/3] the suites that compile generated output =="
fail=0
failed=""
warned=""
while read -r suite; do
    [ -n "$suite" ] || continue
    runner="$(echo tests/"$suite"/run-*.sh)"
    suite_log="$BUILD_DIR/$suite.log"
    echo "-- $runner"
    # Each of these configures a tree of its own, so the warning gate above cannot see
    # theirs; tee so it can.
    if bash "$runner" 2>&1 | tee "$suite_log"; then
        echo "PASS $runner"
    else
        echo "FAIL $runner"
        fail=1
        failed="$failed $runner"
    fi
    if grep -q "CMake Warning" "$suite_log"; then
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
