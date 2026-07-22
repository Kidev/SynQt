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
echo "== [1/4] Materialize the gavel topology and run appgen over it =="
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

echo "== [2/4] Configure + build every entity with the native host kit =="
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build"

echo "== [3/4] Assert each generated entity produced a native executable =="
rc=0
for entity in client web database; do
    assert_native_exe "$SRC/build/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi

echo "== [4/4] A generated client with routes: build it, and watch the router resolve them =="
# Compiling is not enough for URL routing. Every route's view has to be IN the client's QML
# module, or its qrc URL resolves to nothing and the router reports Error on a bundle that
# built perfectly. Only running it says which happened, so this phase runs it.
ROUTED="$WORK/routed"
cp -r "$REPO_ROOT/tests/appgen-native/routed" "$ROUTED"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$ROUTED" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen, check

app, repo = Path(sys.argv[1]), sys.argv[2]
ok, messages = check.check_project(app)
for message in messages:
    print("  synqt check:", message)
if not ok:
    raise SystemExit("the routed fixture does not pass synqt check")
config = yaml.safe_load((app / "synqt.yaml").read_text())
print("  appgen wrote:", ", ".join(appgen.generate(app, config, synqt_root=repo)))
PY

cmake -S "$ROUTED" -B "$ROUTED/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROUTED/build" --target client

routed_exe="$(native_exe_path "$ROUTED/build/client")"
if [ -z "$routed_exe" ]; then
    echo "  routed client : MISSING"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
# The fixture's Main.qml renders Router.pageComponent, reports what resolved, walks to the
# second route, reports again, and quits. It is a real desktop run of the same client
# runtime the browser gets, with no edge and no browser needed.
routed_log="$WORK/routed-run.log"
QT_QPA_PLATFORM=offscreen "$routed_exe" >"$routed_log" 2>&1 || true
sed 's/^/  /' "$routed_log"
for expected in "SYNQT-ROUTE path=/ status=Ready view=Home" \
                "SYNQT-ROUTE path=/about status=Ready view=About"; do
    if ! grep -qF "$expected" "$routed_log"; then
        echo "  expected the routed client to report: $expected"
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1
    fi
done
echo "  routed client : OK (both routes resolved Ready, each to the view it names)"

echo "APPGEN-NATIVE GATE: GO (appgen output compiles and links for every entity, and a"
echo "                       generated client resolves every declared route to its view)"
