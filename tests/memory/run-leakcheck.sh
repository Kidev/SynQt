#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Run every suite in the tree and report what it leaves behind:
#
#   soak      each suite at two repeat counts, comparing the peak resident set;
#   sanitize  the suites rebuilt with AddressSanitizer; fails on a leak record rooted in src/.
#
# Both build with tests/run-all.sh's flags. Usage:
#
#   tests/memory/run-leakcheck.sh                # both passes
#   tests/memory/run-leakcheck.sh --soak         # no instrumented rebuild
#   tests/memory/run-leakcheck.sh --sanitize
#   tests/memory/run-leakcheck.sh --benchmarks   # add the benchmark harnesses to the soak
#
# QT_HOST overrides the kit path.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
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

# tests/run-all.sh's configure line. SYNQT_DEV_TOOLS is required: tests/m8-auth compiles
# against the stub identity server.
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

# Keep ninja's output: it carries the compiler's diagnostics.
build_tree() { # directory
    local directory="$1"
    if cmake --build "$directory" > "$directory/leakcheck-build.log" 2>&1; then
        return
    fi
    echo "error: building $directory failed" >&2
    # Print the failing blocks, since ninja interleaves in-flight jobs; the tail is the fallback.
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
    # -u, so each row prints as it is measured.
    "$PYTHON" -u tests/memory/leakcheck.py soak "$BUILD_DIR" || status=1
fi

if [ "$run_benchmarks" = 1 ]; then
    # The benchmark harnesses whose work scales with the command line. capstone, edge, fanout
    # and mesh have a large fixed cost and are listed as excluded.
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
        # Ten times the work must not cost ten times the memory.
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
    # fast_unwind_on_malloc=0: Qt is built without frame pointers.
    (
        cd "$ASAN_DIR"
        ASAN_OPTIONS="detect_leaks=1:fast_unwind_on_malloc=0:malloc_context_size=40:log_path=$REPO_ROOT/$LOG_DIR/asan" \
            ctest -j"$(nproc)" > /dev/null 2>&1 || true
    )
    "$PYTHON" -u tests/memory/leakcheck.py sanitize "$LOG_DIR" || status=1
fi

exit "$status"
