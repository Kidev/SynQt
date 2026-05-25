#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Coverage for both halves of the framework: the C++ libraries under src/, through the
# host-kit suites on an instrumented build, and the Python CLI, through its own tests.
#
#   QT_HOST=/path/to/qt/gcc_64 tests/run-coverage.sh
#
# BUILD_DIR moves the tree (default build/coverage). CXX_FLOOR, PY_FLOOR and PY_FLOOR_NO_QT
# are the floors; raise them when coverage improves, never lower them. HALVES is `both`
# (default), `cxx` or `py`. PY_FLOOR_NO_QT applies when the CLI finds no QML tools, since
# the tests that drive them then skip.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
BUILD_DIR="${BUILD_DIR:-build/coverage}"
CXX_FLOOR="${CXX_FLOOR:-78}"
PY_FLOOR="${PY_FLOOR:-91}"
PY_FLOOR_NO_QT="${PY_FLOOR_NO_QT:-90}"
HALVES="${HALVES:-both}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=tests/lib/compiler-cache.sh
. "$REPO_ROOT/tests/lib/compiler-cache.sh"

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
# Debug, so each line maps to its own code. --coverage comes from cmake/SynQtCoverage.cmake.
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DSYNQT_COVERAGE=ON \
    -DSYNQT_DEV_TOOLS=ON
cmake --build "$BUILD_DIR"

echo
echo "== [2/4] run the suites against it =="
# Start with no counters from an earlier run.
find "$BUILD_DIR" -name '*.gcda' -delete
# Serial, and a failing suite still reports; the exit status comes at the end.
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
# Ask the CLI which QML tools it finds, from PATH or the resolved kit, as `synqt check` does.
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
    # Write the JSON first: the subshell's status must be the floor check's.
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
