#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The coverage number for both halves of the framework, in one command: the C++ runtime
# libraries under src/, measured by running the host-kit suites against an instrumented
# build, and the Python CLI under tools/synqt/, measured by running its own tests.
#
#   QT_HOST=/path/to/qt/gcc_64 tests/run-coverage.sh
#
# It is deliberately not part of tests/run-all.sh. Coverage needs its own build tree
# (instrumented, and -O0 so a line maps to the code that is actually on it), so folding it
# into the ordinary run would double every build for a number nobody asked for. This is the
# command you run when the number is the question.
#
# BUILD_DIR moves the tree (default build/coverage). CXX_FLOOR and PY_FLOOR are the
# percentages below which this fails; they are the ratchet, so raise them when the number
# goes up and never lower them to make a branch green. HALVES picks which of the two to
# measure (`both`, the default, or `cxx` or `py`), which is how CI runs each half in the
# job that already has what it needs: the C++ half needs a Qt kit, the Python half does not.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
BUILD_DIR="${BUILD_DIR:-build/coverage}"
CXX_FLOOR="${CXX_FLOOR:-78}"
PY_FLOOR="${PY_FLOOR:-89}"
HALVES="${HALVES:-both}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

case "$HALVES" in
    both) do_cxx=1; do_py=1 ;;
    cxx)  do_cxx=1; do_py=0 ;;
    py)   do_cxx=0; do_py=1 ;;
    *) echo "error: HALVES must be both, cxx, or py (got '$HALVES')" >&2; exit 2 ;;
esac

export QT_FORCE_STDERR_LOGGING=1   # see the note in tests/run-all.sh

suites_ok=0
cxx_ok=0
py_ok=0

if [ "$do_cxx" -eq 1 ]; then

echo "== [1/4] configure and build an instrumented tree =="
# Debug, not RelWithDebInfo: at -O2 the optimizer merges and moves lines, so a report over
# an optimized build is a report about the object code rather than about the source anyone
# reads. --coverage itself comes from cmake/SynQtCoverage.cmake.
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSYNQT_COVERAGE=ON
cmake --build "$BUILD_DIR"

echo
echo "== [2/4] run the suites against it =="
# Counters from an earlier run would be added to this one's, reporting a line as reached
# by a suite that no longer reaches it. Start from nothing.
find "$BUILD_DIR" -name '*.gcda' -delete
# Serial, and failures do not stop the report: a suite that fails still leaves counters,
# and a coverage run that refuses to say anything because one test is red is a coverage run
# nobody uses while fixing it. The exit status is reported at the end.
ctest --test-dir "$BUILD_DIR" --output-on-failure || suites_ok=$?
if [ "$suites_ok" -ne 0 ]; then
    echo "warning: the suites did not all pass; the figures below are from a red tree" >&2
fi

echo
echo "== [3/4] C++ line coverage (src/) =="
python3 tools/coverage/report.py \
    --build-dir "$BUILD_DIR" \
    --source-root src \
    --json "$BUILD_DIR/coverage-cxx.json" \
    --fail-under "$CXX_FLOOR" || cxx_ok=$?

fi  # do_cxx

if [ "$do_py" -eq 1 ]; then

echo
echo "== [4/4] Python coverage (tools/synqt/) =="
mkdir -p "$BUILD_DIR"
(
    cd tools/synqt
    python3 -m coverage erase
    python3 -m coverage run -m pytest tests -q
    python3 -m coverage report --fail-under="$PY_FLOOR"
    python3 -m coverage json -o "$REPO_ROOT/$BUILD_DIR/coverage-py.json"
) || py_ok=$?

fi  # do_py

echo
echo "== summary =="
status=0
if [ "$do_cxx" -eq 1 ]; then
    echo "report: $BUILD_DIR/coverage-cxx.json"
    [ "$suites_ok" -eq 0 ] || { echo "FAIL the C++ suites"; status=1; }
    [ "$cxx_ok" -eq 0 ] || { echo "FAIL C++ coverage floor ($CXX_FLOOR%)"; status=1; }
fi
if [ "$do_py" -eq 1 ]; then
    echo "report: $BUILD_DIR/coverage-py.json"
    [ "$py_ok" -eq 0 ] || { echo "FAIL Python coverage floor ($PY_FLOOR%)"; status=1; }
fi
[ "$status" -eq 0 ] && echo "PASS"
exit "$status"
