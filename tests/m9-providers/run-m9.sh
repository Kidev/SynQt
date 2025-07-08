#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# M9: the family interfaces and bundled providers (persistence sqlite/postgres/mysql with a
# bounded connection pool, cache, document, gateway/jobs helpers). Native host kit.
#
# The live provider proofs run only when their SYNQT_TEST_* env vars name a reachable server;
# each skips cleanly otherwise. To run all three against throwaway engines:
#
#   docker run --rm -d --name synqt-pg -e POSTGRES_PASSWORD=synqt \
#       -e POSTGRES_USER=synqt -e POSTGRES_DB=synqt -p 5432:5432 postgres:16
#   docker run --rm -d --name synqt-redis -p 6379:6379 redis:7
#   docker run --rm -d --name synqt-mongo -p 27017:27017 mongo:7
#   export SYNQT_TEST_PG_HOST=127.0.0.1 SYNQT_TEST_PG_PORT=5432 \
#       SYNQT_TEST_PG_DB=synqt SYNQT_TEST_PG_USER=synqt SYNQT_TEST_PG_PASSWORD=synqt
#   export SYNQT_TEST_REDIS_HOST=127.0.0.1 SYNQT_TEST_REDIS_PORT=6379
#   export SYNQT_TEST_MONGO_URI=mongodb://127.0.0.1:27017 SYNQT_TEST_MONGO_DB=synqt
#   tests/m9-providers/run-m9.sh
#   docker rm -f synqt-pg synqt-redis synqt-mongo
#
# The postgres swap (PROV-1) needs the QPSQL plugin's libpq; the redis test needs SynQt built
# with hiredis; the mongodb test needs the MongoDB C driver (mongo-c-driver). A provider not
# compiled in, or an engine not reachable, skips rather than fails. See .run-for-me.sh for a
# one-shot driver that installs the client libraries and starts the engines.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/m9-providers -B build/m9-providers -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/m9-providers
ctest --test-dir build/m9-providers --output-on-failure
