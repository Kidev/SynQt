#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Prove the app generator (tools/synqt appgen) emits code that actually COMPILES, end to end,
# on the native host kit. The appgen unit tests assert the generated strings; this fixture goes
# further and builds them, which is the only thing that catches a missing include or a CMake
# collision. It found three real defects the string tests could not:
#   * the root CMake added SynQtProviders a second time (binary-dir collision); SynQtService
#     already pulls it in;
#   * the service main used QJsonObject with only <QJsonDocument> included (forward-declared);
#   * the edge main upcast QQmlPropertyMap* to QObject* without <QQmlPropertyMap>.
#
# It runs appgen over the real three-entity gavel topology (client + web edge + persistence
# database, with connect points, per_session, identity, and a provider), then configures and
# builds every entity with the native kit. The client's `targets: [wasm]` also builds as a
# native desktop app here, which exercises the client main too. A green run means the generator
# produces buildable code for the full service/edge/provider path.
#
# Needs the pinned host kit (/opt/Qt/6.11.1/gcc_64). Usage:
#   tests/appgen-native/run-appgen-native.sh

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=../lib/native-binary.sh
. "$REPO_ROOT/tests/lib/native-binary.sh"

if [ ! -x "$QT_HOST/bin/qmake" ] && [ ! -d "$QT_HOST/lib/cmake" ]; then
    echo "error: native host kit not found at $QT_HOST" >&2
    exit 1
fi

WORK="$REPO_ROOT/build/appgen-native"
SRC="$WORK/gavel"
echo "== [1/3] Materialize the gavel topology and run appgen over it =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$REPO_ROOT/examples/gavel" "$SRC"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$SRC" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen

app, repo = Path(sys.argv[1]), sys.argv[2]
config = yaml.safe_load((app / "synqt.yaml").read_text())
written = appgen.generate(app, config, synqt_root=repo)
print("  appgen wrote:", ", ".join(written))
PY

echo "== [2/3] Configure + build every entity with the native host kit =="
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build"

echo "== [3/3] Assert each generated entity produced a native executable =="
rc=0
for entity in client web database; do
    assert_native_exe "$SRC/build/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
echo "APPGEN-NATIVE GATE: GO (appgen output compiles and links for every entity)"
