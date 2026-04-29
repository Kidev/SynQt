#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The whole host-kit test story in one command. Configure and build the framework and every
# suite in one tree, run them under one ctest, then run the suites whose entry point is a
# generator rather than CMake (tests/CMakeLists.txt publishes that list. This script does
# not keep its own copy of it).
#
# The per-suite run-*.sh scripts still work and are still the right tool for one milestone
# at a time. This is the whole thing, and it is what CI runs.
#
#   QT_HOST=/path/to/qt/gcc_64 tests/run-all.sh
#
# BUILD_DIR overrides where the tree lands (default build/all), and SYNQT_PHASES runs one
# phase instead of all three (see the block below it).

set -euo pipefail

# Which phases to run. `all` is the default and is what a developer gets by typing
# `tests/run-all.sh`; CI splits the phases across parallel jobs so its wall clock is the
# longest phase rather than their sum. Validated rather than defaulted-on-typo: a job that
# asked for "generated" and got the whole tree instead would pass while proving nothing
# about the suites it was meant to run.
SYNQT_PHASES="${SYNQT_PHASES:-all}"
case "$SYNQT_PHASES" in
all | tree | generated) ;;
*)
    echo "error: SYNQT_PHASES must be all, tree, or generated (got '$SYNQT_PHASES')" >&2
    exit 2
    ;;
esac

# The host kit directory is named for the host, not for what it builds, so one Linux default
# makes this fail on macOS looking for a kit that was never going to be there.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.11.1/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.11.1/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
BUILD_DIR="${BUILD_DIR:-build/all}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# One run of this script compiles the framework about ten times over, so it is worth the
# compiler cache being able to see that. See the file for what it turns off and why.
# shellcheck source=tests/lib/compiler-cache.sh
. "$REPO_ROOT/tests/lib/compiler-cache.sh"

# On Windows a QtTest binary writes its entire log to the debugger, not to standard output,
# whenever it is not attached to a console: QPlainTestLogger::outputMessage() calls
# OutputDebugStringA() and RETURNS, writing nothing to the stream
# (qtbase/src/testlib/qplaintestlogger.cpp). ctest runs every test through a pipe, so
# QtPrivate::shouldLogToStderr() is false there and the whole log disappears, which is why
# every Windows failure this project has seen reported as "***Failed" with a blank capture,
# and why re-running the binary by hand printed nothing either. The one documented override
# is this variable (qtbase/src/corelib/global/qlogging.cpp). Export it for the suites and for
# the phase [3/3] runners, which inherit it. It is a no-op on Linux and macOS, where the
# default handler writes to stderr regardless.
export QT_FORCE_STDERR_LOGGING=1

mkdir -p "$BUILD_DIR"
log="$BUILD_DIR/configure-build.log"

if [ "$SYNQT_PHASES" = "generated" ]; then
    echo "== [1/3] configure the tree (no build: see below) =="
else
    echo "== [1/3] configure and build the tree =="
fi
# pipefail is set, so the exit status is cmake's and not tee's. Without it a failing
# configure would look like a pass.
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tee "$log"
# The configure step is what writes $BUILD_DIR/script-suites.txt, so it runs in every phase
# and CI never holds a second copy of the suite list. The build is what phase [2/3] needs;
# the generated-output suites each configure a tree of their own from $REPO_ROOT and link
# nothing out of this one, so building it for them would be a second full compile of the
# framework for no test.
if [ "$SYNQT_PHASES" != "generated" ]; then
    cmake --build "$BUILD_DIR" 2>&1 | tee -a "$log"
fi

# A CMake warning is a defect, not decoration. The two this gate was built for were a real
# incomplete-linking report (a SynQt library publicly links a Qt module the consumer's scope
# could not resolve) and a Qt module missing from the kit, and both scrolled past in green
# builds for as long as the workflow existed.
if grep -q "CMake Warning" "$log"; then
    echo "error: the tree configured with CMake warnings (see $log)" >&2
    grep -n -A3 "CMake Warning" "$log" >&2
    exit 1
fi

# Phase [2/3], guarded but not indented. The block contains a quoted heredoc,
# and bash only accepts its terminator at column zero, so indenting the body would end the
# heredoc nowhere. The `fi` below carries the condition again so the pair reads at a glance.
if [ "$SYNQT_PHASES" != "generated" ]; then

echo
echo "== [2/3] run the suites =="
# Serial, because several suites bind fixed ports and start real servers, so -j needs an
# audit of what they listen on rather than only a flag.
if ! ctest --test-dir "$BUILD_DIR" --output-on-failure; then
    # ctest prints a failing test's captured output and nothing else, so a test that exits
    # non-zero having written nothing reports as a blank line. The usual cause of that on
    # Windows was the logger redirection the export at the top of this file now disables;
    # what is left is a process that did die before it could say anything, and there
    # the exit code is the whole diagnosis (0xc0000135 is a missing DLL, 0xc0000005 an access
    # violation, a small number QTest's count of failed test functions). ctest never prints
    # it, so run each failing test again for it. Only failing tests are re-run, and each is
    # given a deadline, so a test that failed by hanging cannot hang this too.
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
    # CMake's file(WRITE) opens the stream in text mode, so on Windows the "\n" the
    # registry writes reaches this loop as "\r\n" and every suite name carries a trailing
    # carriage return. Bash keeps it, the glob below then matches nothing, and the failure
    # reads as three suites whose run-*.sh does not exist, on a checkout where all three
    # are present. Strip it here rather than writing the file differently: the reader is
    # the side that knows it wants a line, and this costs nothing on the other platforms.
    suite="${suite%$'\r'}"
    [ -n "$suite" ] || continue
    runner="$(echo tests/"$suite"/run-*.sh)"
    suite_log="$BUILD_DIR/$suite.log"
    echo "-- $runner"
    # Each of these configures a tree of its own, so the warning gate above cannot see
    # theirs. Tee so it can.
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
