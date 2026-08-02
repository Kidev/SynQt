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
# Two fixtures then go past compiling and RUN what was generated, because their claims are not
# about the compiler: `routed/` says every declared route resolves to its view, and `promoted/`
# says `identity.provider_entity` moves the client secret and the token exchange off the edge.
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

# Point the tooling's resolver at the same kit, the way tests/desktop-client does. The
# `synqt check` calls below find qmllint on PATH or in the resolved kit's bin; on CI the
# kit is on neither, so without this the QML lint reports "qmllint not found" and skips,
# and a fixture written to lint a generated app lints nothing. QTDIR is the product's own
# documented escape hatch for exactly this.
export QTDIR="$QT_HOST"

WORK="$REPO_ROOT/build/appgen-native"
SRC="$WORK/gavel"
echo "== [1/5] Materialize the gavel topology and run appgen over it =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$REPO_ROOT/examples/gavel" "$SRC"
# The example is a working directory for whoever has run `synqt build` in it, and those
# leftovers are gitignored, so a fresh clone never has them and a developer's checkout
# does. Copying them in points a fresh configure at a cache built for another source tree,
# and the link step writes an executable over the `database/` directory it copied along.
# Take the tracked sources, not the state.
rm -rf "$SRC/build" "$SRC/CMakeUserPresets.json"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$SRC" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen

app, repo = Path(sys.argv[1]), sys.argv[2]
config = yaml.safe_load((app / "synqt.yaml").read_text())
written = appgen.generate(app, config, synqt_root=repo)
print("  appgen wrote:", ", ".join(written))
PY

echo "== [2/5] Configure + build every entity with the native host kit =="
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build"

echo "== [3/5] Assert each generated entity produced a native executable =="
rc=0
for entity in client web database; do
    assert_native_exe "$SRC/build/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi

echo "== [4/5] A generated client with routes: build it, and watch the router resolve them =="
# Compiling is not enough for URL routing. Every route's view has to be IN the client's QML
# module, and so does everything a view reaches (a helper component, a singleton), or the
# qrc URL resolves to nothing and the router reports Error on a bundle that built perfectly.
# Only running it says which happened, so this phase runs it.
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
# The fixture's Main.qml renders Router.pageComponent, reports what resolved, walks the rest
# of the route table reporting each time, and quits. It is a real desktop run of the same
# client runtime the browser gets, with no edge and no browser needed.
routed_log="$WORK/routed-run.log"
QT_QPA_PLATFORM=offscreen "$routed_exe" >"$routed_log" 2>&1 || true
sed 's/^/  /' "$routed_log"
# Home names itself out of a helper component (Panel.qml) and a singleton (Theme.qml),
# neither of which any route names: they reach the QML module only because every QML file
# under the client entity is compiled in, so "Home(panel,dark)" is that proof. /help is a
# view in a subdirectory, aliased into the module at that same relative path.
for expected in "SYNQT-ROUTE path=/ status=Ready view=Home(panel,dark)" \
                "SYNQT-ROUTE path=/about status=Ready view=About" \
                "SYNQT-ROUTE path=/help status=Ready view=Help"; do
    if ! grep -qF "$expected" "$routed_log"; then
        echo "  expected the routed client to report: $expected"
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1
    fi
done
# A clean run prints those three lines and nothing else. Every QML diagnostic names a file
# and a line ("qrc:/qt/qml/Routed/Main.qml:45: TypeError: Cannot read property
# 'pageComponent' of null"), which is the shape to look for: the generated main used to
# print exactly that on every clean exit, because the accessors its bindings name were
# context properties destroyed while the root object still held bindings on them. Nothing
# failed, so nothing caught it for as long as this phase only looked for lines it wanted.
if grep -nE '\.qml:[0-9]+:' "$routed_log"; then
    echo "  the routed client logged a QML diagnostic; a clean run reports only its routes"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
echo "  routed client : OK (every route resolved Ready, each to the view it names)"

echo "== [5/5] Promoted identity: one line moves the OAuth engine off the edge =="
# `identity.provider_entity: auth` is documented as a one-line change, so everything else it
# needs is generated: two mesh connect points nobody declared, a Source QML bridge for each,
# an auth main holding the OAuth engine and the authoritative session store, and an edge main
# that adopts both Replicas in C++. Compiling that is necessary and not sufficient, because
# the claim is about WHERE a secret lives, so this phase runs the pair and asks the edge for a
# login it cannot answer by itself.
PROMOTED="$WORK/promoted"
cp -r "$REPO_ROOT/tests/appgen-native/promoted" "$PROMOTED"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$PROMOTED" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen, check, mesh, topologywriter

app, repo = Path(sys.argv[1]), sys.argv[2]
ok, messages = check.check_project(app)
for message in messages:
    print("  synqt check:", message)
if not ok:
    raise SystemExit("the promoted fixture does not pass synqt check")
config = yaml.safe_load((app / "synqt.yaml").read_text())
print("  appgen wrote:", ", ".join(appgen.generate(app, config, synqt_root=repo)))
# A real project mesh, because both links are mutual TLS like any other: the auth entity is
# reached over a verified link or not at all.
mesh.init(app)
print("  " + mesh.cert_all(app, ["web", "auth"]).replace("\n", "\n  "))
print("  topology:", ", ".join(topologywriter.write(app, config)))
PY

# Out of tree, because topologywriter owns build/<entity>/ for the resolved topology and the
# generated CMake puts each executable at the top of its own binary directory.
cmake -S "$PROMOTED" -B "$PROMOTED/out" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$PROMOTED/out"

rc=0
for entity in client web auth; do
    assert_native_exe "$PROMOTED/out/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
# Resolve the two the byte search below reads, because it reads them from Python and Python
# is a native Windows program with no MSYS exe magic: `out/web` is a path bash can stat and
# run, and a plain FileNotFoundError to open(). Every needle then reports as absent, so the
# leak check passes vacuously while the presence check fails, which is how a correct build
# came back NO-GO with four tracebacks.
promoted_web="$(native_exe_path "$PROMOTED/out/web")"
promoted_auth="$(native_exe_path "$PROMOTED/out/auth")"

mkdir -p "$PROMOTED/build/client"
printf '<!doctype html>\n' > "$PROMOTED/build/client/index.html"
# Every `kill` swallows its own failure: the script runs under `set -e`, and a cleanup that
# fails because the thing was already gone would turn a green run into a red one.
cleanup_promoted() {
    kill "${auth_pid:-}" "${edge_pid:-}" 2>/dev/null || true
}
trap cleanup_promoted EXIT
# The secret is never a literal in either binary; it arrives from the environment of the one
# entity that runs the token exchange, which is the whole point of the promotion.
export GITHUB_CLIENT_SECRET="appgen-native-not-a-real-secret"
export QT_QPA_PLATFORM=offscreen
# `exec` so the subshell is replaced by the entity: $! is then the process itself, and the
# cleanup above actually stops it instead of stopping a shell that was wrapping it.
(cd "$PROMOTED" && exec ./out/auth >"$WORK/promoted-auth.log" 2>&1) &
auth_pid=$!
sleep 2
# --dev only for the plaintext loopback listener: the fixture's TLS certificate names a
# deployed host, exactly as a real project's does.
(cd "$PROMOTED" && exec ./out/web --bundle build/client --qml-dir . \
    --port 18443 --dev >"$WORK/promoted-web.log" 2>&1) &
edge_pid=$!

# Wait for the login to become answerable rather than for a fixed time: it can only be
# answered once both mesh links are up and both Replicas are adopted.
promoted_login=""
for _ in $(seq 1 30); do
    promoted_login="$(PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - <<'PY'
import urllib.request
request = urllib.request.Request("http://127.0.0.1:18443/auth/login?provider=github")
class Keep(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None
try:
    with urllib.request.build_opener(Keep).open(request, timeout=2) as reply:
        print("%d %s" % (reply.status, reply.headers.get("Location", "")))
except urllib.error.HTTPError as error:
    print("%d %s" % (error.code, error.headers.get("Location", "")))
except Exception:
    print("")
PY
)"
    case "$promoted_login" in
        302*github.com*) break ;;
    esac
    sleep 1
done
echo "  login  -> ${promoted_login:-<no answer>}"
case "$promoted_login" in
    302*"https://github.com/login/oauth/authorize"*"code_challenge"*"state="*) ;;
    *)
        echo "  the edge must answer the login with a PKCE authorization redirect built"
        echo "  from what the auth entity holds; see $WORK/promoted-{auth,web}.log"
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1 ;;
esac

# The redirect above carries a client id and an authorize URL the edge binary does not
# contain. Both encodings are searched (UTF-16 is what a QStringLiteral compiles to; the
# narrow literal it was written as can survive too), because a check that looked at one of
# them would report absence it never established.
#
# A raw byte search in Python, not `strings`: the flag that selects the 16-bit encoding is
# a GNU binutils extension, and Apple's `strings` rejects `-el` outright, so on macOS the
# UTF-16 half of this search produced nothing at all and the gate failed a build that was
# correct. (Windows has no `strings` to begin with; tests/desktop-client scans the same way
# and for the same reason.) Searching bytes is also stricter than `strings`, which only
# reports runs of printable characters above a minimum length.
#
# An unreadable file is a hard error, not an answer. This search is asked both ways round --
# "the edge must not contain it" and "the auth entity must", so a path that cannot be opened
# would otherwise read as absence and quietly satisfy half the checks it was given.
promoted_in_binary() {
    if [ ! -f "$1" ]; then
        echo "  cannot read $1, so nothing here was established" >&2
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1
    fi
    SYNQT_BIN="$1" SYNQT_NEEDLE="$2" python3 - <<'PY'
import os
import sys

data = open(os.environ["SYNQT_BIN"], "rb").read()
needle = os.environ["SYNQT_NEEDLE"]
found = needle.encode("utf-8") in data or needle.encode("utf-16-le") in data
sys.exit(0 if found else 1)
PY
}
promoted_leak=0
for needle in "Iv1.0123456789abcdef" "github.com/login/oauth" "$GITHUB_CLIENT_SECRET"; do
    if promoted_in_binary "$promoted_web" "$needle"; then
        echo "  the edge binary must not contain '$needle'"
        promoted_leak=1
    fi
done
# The same two values in the auth binary: without this the check above would also pass on a
# generator that simply dropped the provider, which is a broken login, not a secure one.
for needle in "Iv1.0123456789abcdef" "github.com/login/oauth"; do
    if ! promoted_in_binary "$promoted_auth" "$needle"; then
        echo "  the auth binary is missing '$needle', so the redirect came from somewhere else"
        promoted_leak=1
    fi
done
if promoted_in_binary "$promoted_auth" "$GITHUB_CLIENT_SECRET"; then
    echo "  the auth binary must read the secret from its environment, never carry it"
    promoted_leak=1
fi
if [ "$promoted_leak" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
cleanup_promoted
echo "  promoted pair : OK (both mesh links up, the edge holds no client id, no provider"
echo "                  endpoint and no secret; the auth entity holds the first two and"
echo "                  reads the secret from its own environment)"

echo "APPGEN-NATIVE GATE: GO (appgen output compiles and links for every entity, a"
echo "                       generated client resolves every declared route to its view,"
echo "                       and a promoted identity signs in from the auth entity)"
