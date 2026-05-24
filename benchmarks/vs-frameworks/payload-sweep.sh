#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The same fan-out, refitted at six payload sizes, to say what a stack's per-subscriber cost
# is made of.
#
# The headline sweep varies the number of subscribers and holds the payload at 256 bytes.
# That is the right shape for the comparison and it cannot answer one question: whether the
# marginal cost a column pays per subscriber is the bytes being copied for that subscriber
# or a syscall, a wakeup and a dispatch that would cost the same if the frame were empty.
# The two want opposite work. Sweeping the payload separates them, because a copy grows with
# the payload and an overhead does not.
#
#   bash benchmarks/vs-frameworks/payload-sweep.sh
#   bash benchmarks/vs-frameworks/payload-sweep.sh --seconds 8 --subscribers 10,50,100,250,500
#
# Three columns rather than sixteen. This is the instrument that reads the gap in the
# comparison table, and the gap is between SynQt, the same fan-out with the object
# protocol taken off (`qt-raw`), and the fastest Node. Every other column would add
# an hour to the run to answer a question nobody asked of it.
#
# Saturating, because the fit under it is only defined on a saturating run: a paced run
# spends the interval idle and the line through its points has no fixed cost in it. See
# fit.py, which is what reads the output.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-vs-frameworks"
NODE_DIR="benchmarks/vs-frameworks/node"
# Not under benchmarks/results/, which is where the committed baselines live: this is an
# instrument reading rather than a baseline, and two files of one benchmark in that directory
# would make "the baseline" ambiguous. BENCH_OUT_DIR moves it, and the local runner does.
OUT_DIR="${BENCH_OUT_DIR:-$REPO_ROOT/build/bench-payload-sweep}"

# The sizes, and why these six. 64 is smaller than any header a stack puts around it, so it
# is as close to "no payload" as a real frame gets; 256 is what the comparison table runs at
# and is the anchor the two tables share. The rest double-and-double to 64 KiB, which is
# about where a model replication of a screenful of rows lands. Six points across three
# orders of magnitude is what makes the slope against payload readable rather than a pair.
PAYLOADS="${PAYLOADS:-64 256 1024 4096 16384 65536}"
SUBSCRIBERS="10,50,100,250"
SECONDS_PER_SIZE="5"

while [ $# -gt 0 ]; do
    case "$1" in
        --subscribers) SUBSCRIBERS="$2"; shift 2 ;;
        --seconds) SECONDS_PER_SIZE="$2"; shift 2 ;;
        --payloads) PAYLOADS="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# The LTS, the first major in runtimes.txt. The Node column here is the floor the two Qt
# columns are read against, and the floor a team is allowed to deploy is the one to
# put a fix decision on. Not `node`, which is whichever version a shell has selected.
node_major="$(sed 's/#.*//' "$NODE_DIR/runtimes.txt" | grep -o '[0-9]\+' | head -1)"
node_bin=""
node_root="${NVM_DIR:-$HOME/.nvm}/versions/node"
node_newest="$(ls -d "$node_root"/v"$node_major".* 2>/dev/null | sed 's|.*/v||' | sort -V | tail -1 || true)"
if [ -n "$node_newest" ] && [ -x "$node_root/v$node_newest/bin/node" ]; then
    node_bin="$node_root/v$node_newest/bin/node"
elif command -v node >/dev/null 2>&1 &&
     [ "$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || true)" = "$node_major" ]; then
    node_bin="$(command -v node)"
fi

echo "== configure + build the Qt columns =="
cmake -S benchmarks/vs-frameworks -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

if [ -z "$node_bin" ]; then
    echo "Node $node_major is not installed; the sweep runs the two Qt columns only" >&2
else
    echo "Node $node_major: $("$node_bin" --version) at $node_bin"
fi

for bytes in $PAYLOADS; do
    size_dir="$OUT_DIR/p$bytes"
    mkdir -p "$size_dir"
    echo
    echo "== payload $bytes bytes =="
    "$BUILD_DIR/bench_live" --subscribers "$SUBSCRIBERS" --seconds "$SECONDS_PER_SIZE" \
        --saturate --payload "$bytes" --out "$size_dir/vs-fw-synqt.json"
    "$BUILD_DIR/bench_live" --raw --subscribers "$SUBSCRIBERS" --seconds "$SECONDS_PER_SIZE" \
        --saturate --payload "$bytes" --out "$size_dir/vs-fw-qtraw.json"
    if [ -n "$node_bin" ]; then
        (cd "$NODE_DIR" && "$node_bin" live-bare.mjs --subscribers "$SUBSCRIBERS" \
            --seconds "$SECONDS_PER_SIZE" --saturate --payload "$bytes" \
            --out "$size_dir/vs-fw-bare.json")
    fi
done

echo
python3 benchmarks/vs-frameworks/fit.py --payloads "$OUT_DIR"
