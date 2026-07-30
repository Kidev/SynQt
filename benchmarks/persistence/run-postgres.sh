#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The persistence workload through the pooled PostgresProvider, twice: over plaintext
# loopback, and over TLS verified the way a release build connects. Each run writes its own
# baseline beside the SQLite one.
#
#   benchmarks/persistence/run-postgres.sh [bench_postgres options]
#
# It measures the server SYNQT_TEST_PG_* names. When none is named it starts the throwaway
# engines tests/lib/live-engines.sh provides, and removes them afterwards.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/bench-persistence"
RESULTS_DIR="benchmarks/results"
HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"

echo "== configure + build =="
cmake -S benchmarks/persistence -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target bench_postgres

started=0
if [ -z "${SYNQT_TEST_PG_HOST:-}" ]; then
    echo "== start the throwaway engines =="
    tests/lib/live-engines.sh up
    started=1
    eval "$(tests/lib/live-engines.sh env)"
fi
stop() {
    if [ "$started" = 1 ]; then
        tests/lib/live-engines.sh down
    fi
}
trap stop EXIT

if [ -z "${SYNQT_TEST_PG_HOST:-}" ]; then
    echo "no PostgreSQL answered; nothing to measure" >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR"
echo "== plaintext loopback =="
"$BUILD_DIR/bench_postgres" --out "$RESULTS_DIR/persistence-postgres-${HOST_TAG}.json" "$@"
echo
echo "== verified TLS =="
"$BUILD_DIR/bench_postgres" --tls \
    --out "$RESULTS_DIR/persistence-postgres-tls-${HOST_TAG}.json" "$@"
