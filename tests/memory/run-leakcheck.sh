#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Run the suites that already exist, and ask what they left behind.
#
# tst_memory (the ctest suite next to this script) measures the paths it was written for.
# This asks the same question of everything else in the tree, in the two ways a leak can
# show itself:
#
#   soak      every suite run at two repeat counts, comparing the peak resident set. This
#             is the half that sees memory the process is still holding on purpose, which
#             is what a leak in a long-running entity looks like from outside.
#   sanitize  the same suites rebuilt with AddressSanitizer and run again, with every leak
#             LeakSanitizer reports charged to whoever allocated it. This is the half that
#             sees memory nothing points at any more, and it fails the run when a record
#             belongs to src/.
#
# Neither is cheap. Both configure and build a tree (the sanitizer pass an instrumented one
# of its own), with the same flags tests/run-all.sh uses, so the binaries measured here are
# the binaries a developer ran. Usage:
#
#   tests/memory/run-leakcheck.sh                # both passes
#   tests/memory/run-leakcheck.sh --soak         # the fast half, no instrumented rebuild
#   tests/memory/run-leakcheck.sh --sanitize
#   tests/memory/run-leakcheck.sh --benchmarks   # add the benchmark harnesses to the soak
#
# QT_HOST overrides the kit path, as in every other runner here.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${SYNQT_BUILD:-build/all}"
ASAN_DIR="build/leakcheck"
LOG_DIR="$ASAN_DIR/leaks"
PYTHON="${PYTHON:-python3}"

run_soak=1
run_sanitize=1
run_benchmarks=0
for argument in "$@"; do
    case "$argument" in
        --soak) run_sanitize=0 ;;
        --sanitize) run_soak=0 ;;
        --benchmarks) run_benchmarks=1 ;;
        *) echo "unknown option: $argument" >&2; exit 2 ;;
    esac
done

status=0

# The one configure line, for both trees below, and it is tests/run-all.sh's flag for flag.
# SYNQT_DEV_TOOLS is not decoration here. Tests/m8-auth compiles against the stub identity
# server, whose header refuses to be included by a build that did not ask for one, so a
# tree configured without the flag fails to compile and no suite runs at all. This script
# printed a shorter configure line and the workflow ran a shorter one, and neither had the
# flag. The whole leak column failed on it.
configure_tree() { # directory, extra cmake arguments...
    local directory="$1"
    shift
    mkdir -p "$directory"
    if ! cmake -S . -B "$directory" -G Ninja \
            -DCMAKE_PREFIX_PATH="$QT_HOST" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DSYNQT_DEV_TOOLS=ON \
            "$@" > "$directory/leakcheck-configure.log" 2>&1; then
        echo "error: configuring $directory failed" >&2
        tail -n 40 "$directory/leakcheck-configure.log" >&2
        exit 2
    fi
}

# ninja writes the compiler's diagnostics to stdout, so `> /dev/null` is not quiet, it is
# blind. That is exactly how this failed in CI: a header line, four and a half minutes, an
# exit code, and not one word about what did not compile.
build_tree() { # directory
    local directory="$1"
    if cmake --build "$directory" > "$directory/leakcheck-build.log" 2>&1; then
        return
    fi
    echo "error: building $directory failed" >&2
    # ninja keeps every job it had in flight running after one of them fails and prints
    # their output as it arrives, so the diagnostics are almost never the last thing in the
    # log. The failing blocks are what there is to read. The tail is the fallback for a
    # failure that was not a compile at all.
    if grep -q "^FAILED:" "$directory/leakcheck-build.log"; then
        grep -A 25 "^FAILED:" "$directory/leakcheck-build.log" | head -n 150 >&2
    else
        tail -n 60 "$directory/leakcheck-build.log" >&2
    fi
    exit 2
}

if [ "$run_soak" = 1 ]; then
    echo "== soak: what each suite keeps per repetition =="
    configure_tree "$BUILD_DIR"
    build_tree "$BUILD_DIR"
    # -u because this is a table printed one row per suite over half an hour, and a
    # block-buffered stdout would hold every row until the last suite finished, which
    # is how a CI log ends up with a header and nothing under it.
    "$PYTHON" -u tests/memory/leakcheck.py soak "$BUILD_DIR" || status=1
fi

if [ "$run_benchmarks" = 1 ]; then
    # The benchmark harnesses, at two workloads each. Only the ones whose work is
    # the number on the command line: capstone, edge and fanout stand up whole systems,
    # and mesh spends minutes on TLS handshakes and a throughput sweep whatever --samples
    # says, so the difference between two settings would be noise on top of a fixed cost.
    # They are named as left out rather than quietly halved.
    echo
    echo "== soak: benchmark harnesses =="
    echo "(not covered here: capstone, edge, fanout, mesh; fixed work dominates the knob)"
    for entry in "sessions:--iterations:20000:400000" "persistence:--autocommit-rows:200:2000"; do
        name="${entry%%:*}"
        rest="${entry#*:}"
        flag="${rest%%:*}"
        rest="${rest#*:}"
        low="${rest%%:*}"
        high="${rest##*:}"
        binary="build/bench-$name/bench_$name"
        if [ ! -x "$binary" ]; then
            echo "$name: not built (benchmarks/$name/run-bench.sh builds it); skipped"
            continue
        fi
        out="$(mktemp)"
        low_rss="$("$PYTHON" - "$binary" "$flag" "$low" "$out" <<'PY'
import os, sys
binary, flag, count, out = sys.argv[1:5]
pid = os.fork()
if pid == 0:
    devnull = os.open(os.devnull, os.O_WRONLY)
    os.dup2(devnull, 1); os.dup2(devnull, 2)
    os.execv(binary, [binary, flag, count, "--out", out])
_, status, usage = os.wait4(pid, 0)
print(usage.ru_maxrss if status == 0 else -1)
PY
)"
        high_rss="$("$PYTHON" - "$binary" "$flag" "$high" "$out" <<'PY'
import os, sys
binary, flag, count, out = sys.argv[1:5]
pid = os.fork()
if pid == 0:
    devnull = os.open(os.devnull, os.O_WRONLY)
    os.dup2(devnull, 1); os.dup2(devnull, 2)
    os.execv(binary, [binary, flag, count, "--out", out])
_, status, usage = os.wait4(pid, 0)
print(usage.ru_maxrss if status == 0 else -1)
PY
)"
        rm -f "$out"
        if [ "$low_rss" = "-1" ] || [ "$high_rss" = "-1" ]; then
            echo "$name: harness did not finish; skipped"
            continue
        fi
        # Ten times the work must not cost ten times the memory. A harness whose peak
        # tracks its iteration count is accumulating per iteration.
        echo "$name: ${low_rss} KB at $low, ${high_rss} KB at $high"
    done
fi

if [ "$run_sanitize" = 1 ]; then
    echo
    echo "== sanitize: LeakSanitizer over the whole tree =="
    configure_tree "$ASAN_DIR" \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
    build_tree "$ASAN_DIR"
    rm -rf "$LOG_DIR"
    mkdir -p "$LOG_DIR"
    # fast_unwind_on_malloc=0 is what makes the report usable: Qt's own libraries are built
    # without frame pointers, so the fast unwinder stops inside them and every allocation
    # Qt makes on SynQt's behalf looks like Qt's own. It costs a few seconds a suite.
    (
        cd "$ASAN_DIR"
        ASAN_OPTIONS="detect_leaks=1:fast_unwind_on_malloc=0:malloc_context_size=40:log_path=$REPO_ROOT/$LOG_DIR/asan" \
            ctest -j"$(nproc)" > /dev/null 2>&1 || true
    )
    "$PYTHON" -u tests/memory/leakcheck.py sanitize "$LOG_DIR" || status=1
fi

exit "$status"
