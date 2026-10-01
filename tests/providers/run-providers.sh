#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The family interfaces and bundled providers. Native host kit.
#
# The live provider proofs run only when their SYNQT_TEST_* variables name a reachable server:
#
#   tests/lib/live-engines.sh up
#   eval "$(tests/lib/live-engines.sh env)"
#   tests/providers/run-providers.sh
#   tests/lib/live-engines.sh down
#
# The mysql proof needs QMYSQL built against MariaDB Connector/C (docs/licensing.md):
#
#   tools/qmysql-plugin/build-qmysql-plugin.sh
#   export QT_PLUGIN_PATH="$HOME/.cache/synqt-qmysql"
#
# postgres needs libpq; redis needs hiredis (hiredis_ssl and OpenSSL for TLS); mongodb needs
# mongo-c-driver. Debian: libpq-dev, libhiredis-dev, libmongoc-dev. Arch: postgresql-libs,
# hiredis (no hiredis_ssl), mongo-c-driver. A missing provider or engine skips.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

cmake -S tests/providers -B build/providers -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/providers
ctest --test-dir build/providers --output-on-failure
