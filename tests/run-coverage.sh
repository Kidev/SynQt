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
# BUILD_DIR moves the tree (default build/coverage). CXX_FLOOR, PY_FLOOR and
# PY_FLOOR_NO_QT are the percentages below which this fails; they are the ratchet, so raise
# them when the number goes up and never lower them to make a branch green. HALVES picks
# which of the two halves to measure (`both`, the default, or `cxx` or `py`), which is how
# CI runs each half in the job that already has what it needs: the C++ half needs a Qt kit,
# the Python half does not.
#
# The Python half has two floors because it measures two different things. A few of its
# tests drive qmllint and qmlformat, and those tools ship with a Qt kit; on a machine that
# has none they skip, so the same suite reaches less code and the number is honestly lower.
# One floor for both environments meant a run with no Qt failed a bar set by a run with it,
# which is a bug in the bar rather than in the tests. Which floor applies is decided by
# asking the CLI itself which tools it can find, so the answer always matches the tests
# that will be skipped.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
BUILD_DIR="${BUILD_DIR:-build/coverage}"
CXX_FLOOR="${CXX_FLOOR:-78}"
PY_FLOOR="${PY_FLOOR:-92}"
PY_FLOOR_NO_QT="${PY_FLOOR_NO_QT:-90}"
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
# Ask the CLI which QML tools it can find, exactly as `synqt check` will: they come from
# PATH or from the resolved Qt kit, so looking only at PATH would report "no Qt" on the
# usual developer machine and apply the lower floor to a run that reached everything.
if (cd tools/synqt && python3 -c "import sys
from synqt import check
sys.exit(0 if (check.qmllint_path() and check.qmlformat_path()) else 1)") 2>/dev/null; then
    py_floor="$PY_FLOOR"
    echo "qmllint and qmlformat are available: enforcing the full floor ($py_floor%)."
else
    py_floor="$PY_FLOOR_NO_QT"
    echo "no qmllint/qmlformat on this machine: their tests will skip, so the floor is" \
         "the one for a run without a Qt kit ($py_floor%)."
fi
(
    cd tools/synqt
    python3 -m coverage erase
    python3 -m coverage run -m pytest tests -q
    # The JSON report is written before the floor is checked, because a subshell exits with
    # the status of its last command: with these two the other way round, `coverage json`
    # succeeding overwrote the `--fail-under` failure and $py_ok was 0 no matter what the
    # number was. The floor printed "Coverage failure: ..." and the run still said PASS, so
    # the Python floor was not enforced at all, here or in CI.
    python3 -m coverage json -o "$REPO_ROOT/$BUILD_DIR/coverage-py.json"
    python3 -m coverage report --fail-under="$py_floor"
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
    [ "$py_ok" -eq 0 ] || { echo "FAIL Python coverage floor (${py_floor}%)"; status=1; }
fi
[ "$status" -eq 0 ] && echo "PASS"
exit "$status"
