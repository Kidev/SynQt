#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Time the build itself: contract generation, and clean, no-op, and touched-file builds per
# entity. Writes a JSON baseline under benchmarks/results/ keyed by hostname. Pass extra
# flags through to the harness, e.g.
#   ./run-bench.sh --project examples/arena --repeats 20
#   ./run-bench.sh --include-client        # also the WASM client; needs the Emscripten kit
#
# QT_HOST overrides the kit path and BENCH_OUT overrides where the baseline is written,
# so CI can run this against its own kit without writing into benchmarks/results/.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
OUT="${BENCH_OUT:-${RESULTS_DIR}/buildtime-${HOST_TAG}.json}"

mkdir -p "$RESULTS_DIR"
QT_HOST="$QT_HOST" python3 benchmarks/buildtime/measure.py --out "$OUT" "$@"
