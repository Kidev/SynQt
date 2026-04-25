#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Prove the app generator (tools/synqt appgen) emits code that COMPILES, end to end,
# on the native host kit. The appgen unit tests assert the generated strings. This fixture goes
# further and builds them, which is the only thing that catches a missing include or a CMake
# collision. It found three real defects the string tests could not:
#   * the root CMake added SynQtProviders a second time (binary-dir collision); SynQtService
#     already pulls it in;
#   * the service main used QJsonObject with only <QJsonDocument> included (forward-declared);
#   * the edge main upcast QQmlPropertyMap* to QObject* without <QQmlPropertyMap>.
#
# It runs appgen over the real three-entity gavel topology (client + web edge + relational
# database, with connect points, a scope-gated point, identity, and a provider), then configures and
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
# `synqt check` calls below find qmllint on PATH or in the resolved kit's bin. On CI the
# kit is on neither, so without this the QML lint reports "qmllint not found" and skips,
# and a fixture written to lint a generated app lints nothing. QTDIR is the product's own
# documented escape hatch for exactly this.
export QTDIR="$QT_HOST"

WORK="$REPO_ROOT/build/appgen-native"
SRC="$WORK/gavel"
echo "== [1/8] Materialize the gavel topology and run appgen over it =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$REPO_ROOT/examples/gavel" "$SRC"
# The example is a working directory for whoever has run `synqt build` in it, and those
# leftovers are gitignored, so a fresh clone never has them and a developer's checkout
# does. Copying them in points a fresh configure at a cache built for another source tree,
# and the link step writes an executable over an entity directory it copied along.
# Take the tracked sources, not the state.
rm -rf "$SRC/build" "$SRC/generated"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$SRC" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen

app, repo = Path(sys.argv[1]), sys.argv[2]
config = yaml.safe_load((app / "synqt.yaml").read_text())
written = appgen.generate(app, config, synqt_root=repo)
print("  appgen wrote:", ", ".join(written))
PY

echo "== [2/8] Configure + build every entity with the native host kit =="
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build"

echo "== [3/8] Assert each generated entity produced a native executable =="
rc=0
for entity in app edge books; do
    assert_native_exe "$SRC/build/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi

echo "== [4/8] A generated client with routes: build it, and watch the router resolve them =="
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
cmake --build "$ROUTED/build" --target app

routed_exe="$(native_exe_path "$ROUTED/build/app")"
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
# neither of which any route names. They reach the QML module only because every QML file
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
# 'pageComponent' of null"), which is the shape to look for. A generated main whose
# accessors are context properties destroyed while the root object still holds bindings
# on them prints exactly that on every clean exit. Nothing fails, so a phase that only
# looks for the lines it wants cannot catch it.
if grep -nE '\.qml:[0-9]+:' "$routed_log"; then
    echo "  the routed client logged a QML diagnostic; a clean run reports only its routes"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
echo "  routed client : OK (every route resolved Ready, each to the view it names)"

echo "== [5/8] Promoted identity: one line moves the OAuth engine off the edge =="
# `identity.provider_entity: auth` is documented as a one-line change, so everything else it
# needs is generated. Two mesh connect points nobody declared, a Source QML bridge for each,
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
print("  " + mesh.cert_all(app, ["edge", "auth"]).replace("\n", "\n  "))
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
for entity in app edge auth; do
    assert_native_exe "$PROMOTED/out/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
# Resolve the two the byte search below reads, because it reads them from Python and Python
# is a native Windows program with no MSYS exe magic. `out/edge` is a path bash can stat and
# run, and a plain FileNotFoundError to open(). Every needle then reports as absent, so the
# leak check passes vacuously while the presence check fails, which is how a correct build
# came back NO-GO with four tracebacks.
promoted_web="$(native_exe_path "$PROMOTED/out/edge")"
promoted_auth="$(native_exe_path "$PROMOTED/out/auth")"

mkdir -p "$PROMOTED/build/client"
printf '<!doctype html>\n' > "$PROMOTED/build/client/index.html"
# Every `kill` swallows its own failure: the script runs under `set -e`, and a cleanup that
# fails because the thing was already gone would turn a green run into a red one.
cleanup_promoted() {
    kill "${auth_pid:-}" "${edge_pid:-}" 2>/dev/null || true
}
trap cleanup_promoted EXIT
# The secret is never a literal in either binary. It arrives from the environment of the one
# entity that runs the token exchange, which is what the promotion buys.
export GITHUB_CLIENT_SECRET="appgen-native-not-a-real-secret"
export QT_QPA_PLATFORM=offscreen
# `exec` so the subshell is replaced by the entity: $! is then the process itself, and the
# cleanup above stops it instead of stopping a shell that was wrapping it.
#
# --dev on the auth entity as well as on the edge, which is what `synqt dev` passes it
# (run.dev_command) and for the reason the check below establishes. The promotion moves the
# token exchange here, so this is the process that reads a provider entry and decides
# whether it may be spoken to. Without the flag it refuses the development sign-in and the
# edge answers the login route with 403, which is exactly what a deployment does.
(cd "$PROMOTED" && exec ./out/auth --dev >"$WORK/promoted-auth.log" 2>&1) &
auth_pid=$!
sleep 2
# --dev only for the plaintext loopback listener. The fixture's TLS certificate names a
# deployed host, exactly as a real project's does. The QML directory is the mirror under
# generated/, which is what `synqt dev` passes and what the edge defaults to: the author's
# tree happens to work for this fixture's own Edge.qml and would not for an entity whose
# root object had to be retyped, so pointing at it here would prove the wrong thing.
(cd "$PROMOTED" && exec ./out/edge --bundle build/client --qml-dir generated \
    --port 18443 --dev >"$WORK/promoted-web.log" 2>&1) &
edge_pid=$!

# Wait for the login to become answerable rather than for a fixed time. It can only be
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
# contain. Both encodings are searched (UTF-16 is what a QStringLiteral compiles to. The
# narrow literal it was written as can survive too), because a check that looked at one of
# them would report absence it never established.
#
# A raw byte search in Python, not `strings`: the flag that selects the 16-bit encoding is
# a GNU binutils extension, and Apple's `strings` rejects `-el` outright, so on macOS the
# UTF-16 half of this search produced nothing at all and the gate failed a build that was
# correct. (Windows has no `strings` to begin with. Tests/desktop-client scans the same way
# and for the same reason.) Searching bytes is also stricter than `strings`, which only
# reports runs of printable characters above a minimum length.
#
# An unreadable file is a hard error, not an answer. This search is asked both ways round,
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
# The same two values in the auth binary. Without this the check above would also pass on a
# generator that dropped the provider, which is a broken login, not a secure one.
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

# The development sign-in, in the arrangement it has the most to prove itself against. The
# server runs inside the edge and the entity that dials it is a different process, so what
# a green answer here says is that the generated edge started it under --dev, that the auth
# entity was given endpoints pointing at it, and that the two agreed on the port and the
# shared secret with nothing between them to agree through. `synqt serve` passes no --dev,
# so the same tree deployed answers this with a 403.
promoted_dev="$(PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - <<'PY'
import urllib.request
request = urllib.request.Request("http://127.0.0.1:18443/auth/login?provider=dev")
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
echo "  dev login -> ${promoted_dev:-<no answer>}"
case "$promoted_dev" in
    302*"http://127.0.0.1:8789/authorize"*"code_challenge"*"state="*) ;;
    *)
        echo "  the edge must answer the development sign-in with an authorization redirect"
        echo "  to the stub it started; see $WORK/promoted-{auth,web}.log"
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1 ;;
esac

# And the stub is answering at the other end of that redirect. Two people are configured,
# so it asks which rather than picking one.
promoted_chooser="$(SYNQT_LOCATION="${promoted_dev#302 }" python3 - <<'PY'
import os
import urllib.request
try:
    with urllib.request.urlopen(os.environ["SYNQT_LOCATION"], timeout=2) as reply:
        body = reply.read().decode("utf-8", "replace")
    print("%d %s" % (reply.status, "both" if ("Developer" in body and "Moderator" in body)
                     else "partial"))
except Exception:
    print("")
PY
)"
echo "  dev chooser -> ${promoted_chooser:-<no answer>}"
case "$promoted_chooser" in
    "200 both") ;;
    *)
        echo "  the development sign-in must offer both configured people"
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1 ;;
esac

cleanup_promoted
echo "  promoted pair : OK (both mesh links up, the edge holds no client id, no provider"
echo "                  endpoint and no secret; the auth entity holds the first two and"
echo "                  reads the secret from its own environment; the development"
echo "                  sign-in runs in the edge and the auth entity reaches it)"

echo "== [6/8] A front: an edge that owns a point it does not implement =="
# A front hands each caller to the entity serving people of their scope, so the edge has no
# server file for that point and the Source the browser acquires relays to a Replica of a
# different contract. Compiling is the check that matters. The generated edge main has to
# build with no server file, its `behind:` block reaches WebEdge through a QSet the generator
# has to remember to include, and the generated relay has to resolve against a contract the
# front's own binary knows only by name.
FRONTED="$WORK/fronted"
cp -r "$REPO_ROOT/tests/appgen-native/fronted" "$FRONTED"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$FRONTED" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen, check

app, repo = Path(sys.argv[1]), sys.argv[2]
ok, messages = check.check_project(app)
for message in messages:
    print("  synqt check:", message)
if not ok:
    raise SystemExit("the fronted fixture does not pass synqt check")
config = yaml.safe_load((app / "synqt.yaml").read_text())
print("  appgen wrote:", ", ".join(appgen.generate(app, config, synqt_root=repo)))
PY

cmake -S "$FRONTED" -B "$FRONTED/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$FRONTED/build"

for entity in gate lobby backoffice; do
    exe="$(native_exe_path "$FRONTED/build/$entity")"
    if [ -z "$exe" ]; then
        echo "  $entity : MISSING"
        exit 1
    fi
    echo "  $entity : $(basename "$exe")"
done
echo "  front : OK (the edge builds with no Source of its own for the point it fronts)"

echo "== [7/8] An entity with a network: block: build it, and call the API it serves =="
# The generated main is what is under test. It has to build an ApiConfig from the topology,
# link SynQtGateway, put `Api` on the root context BEFORE the entity singleton is created
# (or the singleton's routes go nowhere) and start listening AFTER (or a caller can arrive
# at a surface with no routes). None of that is visible in a build, so this phase runs it
# and calls it, with and without the key.
GATEWAY="$WORK/gateway"
cp -r "$REPO_ROOT/tests/appgen-native/gateway" "$GATEWAY"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$GATEWAY" "$REPO_ROOT" <<'PY'
import sys, yaml
from pathlib import Path
from synqt import appgen, check, topologywriter

app, repo = Path(sys.argv[1]), sys.argv[2]
ok, messages = check.check_project(app)
for message in messages:
    print("  synqt check:", message)
if not ok:
    raise SystemExit("the gateway fixture does not pass synqt check")
config = yaml.safe_load((app / "synqt.yaml").read_text())
print("  appgen wrote:", ", ".join(appgen.generate(app, config, synqt_root=repo)))
print("  topology:", ", ".join(topologywriter.write(app, config)))
PY

cmake -S "$GATEWAY" -B "$GATEWAY/out" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$GATEWAY/out"

rc=0
for entity in app edge gw; do
    assert_native_exe "$GATEWAY/out/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi

cleanup_gateway() {
    kill "${gw_pid:-}" 2>/dev/null || true
}
trap cleanup_gateway EXIT
export GW_API_KEYS="appgen-native-key,second-key"
(cd "$GATEWAY" && exec ./out/gw --topology build/gw/topology.json --qml-dir . \
    >"$WORK/gateway-gw.log" 2>&1) &
gw_pid=$!

# The route is handed over as a whole URL rather than as a path, and the variable is not named
# after one. Git Bash's MSYS runtime rewrites a value that looks like an absolute POSIX path
# into a Windows path before a native program sees it, and it treats a *PATH variable as a path
# list besides. Python received 'C:/Program Files/Git/health' where /health was meant, built a
# URL with a space in it and refused all three calls, which failed this suite on the Windows
# column alone. A value starting with http:// is left alone. Same runtime, and the same shape
# of surprise, as the openssl subject in tests/lib/mesh-certs.sh.
gateway_call() {
    SYNQT_KEY="${2:-}" SYNQT_URL="http://127.0.0.1:18456$1" SYNQT_BODY="${3:-}" \
        SYNQT_FORWARDED="${4:-}" python3 - <<'PY'
import json, os, urllib.error, urllib.request

body = os.environ["SYNQT_BODY"].encode() or None
request = urllib.request.Request(os.environ["SYNQT_URL"],
                                 data=body, method="POST" if body else "GET")
request.add_header("Content-Type", "application/json")
if os.environ["SYNQT_KEY"]:
    request.add_header("X-API-Key", os.environ["SYNQT_KEY"])
if os.environ["SYNQT_FORWARDED"]:
    request.add_header("X-Forwarded-For", os.environ["SYNQT_FORWARDED"])
try:
    with urllib.request.urlopen(request, timeout=2) as reply:
        print("%d %s" % (reply.status, reply.read().decode().strip()))
except urllib.error.HTTPError as error:
    print("%d %s" % (error.code, error.read().decode().strip()))
except Exception as error:
    # Named, not swallowed. "<no answer>" reads the same whether the port refused the
    # connection or the entity accepted it and never replied, and those are different
    # bugs. The first says the listener is not up yet, the second says it is up and
    # stuck. A Windows run spent forty seconds on the first while looking like the second.
    print("- %s: %s" % (type(error).__name__, error))
PY
}

# Wait for the entity to say it is listening before asking it anything. The entity's own
# file runs before the listener starts (routes have to exist before a caller can arrive),
# so anything that file does slowly is time the port is closed, and a probe loop alone
# cannot tell that from a gateway that is broken.
gateway_up=0
for _ in $(seq 1 60); do
    if grep -q "gw API on port" "$WORK/gateway-gw.log" 2>/dev/null; then
        gateway_up=1
        break
    fi
    if ! kill -0 "$gw_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
if [ "$gateway_up" -ne 1 ]; then
    echo "  the gateway never reported a listening API surface"
    sed 's/^/  /' "$WORK/gateway-gw.log"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi

gateway_health="$(gateway_call /health appgen-native-key)"
echo "  GET /health with a key    -> ${gateway_health:-<no answer>}"
gateway_nokey="$(gateway_call /health)"
echo "  GET /health with no key   -> ${gateway_nokey:-<no answer>}"
gateway_echo="$(gateway_call /echo/7 appgen-native-key '{"value":"hi"}')"
echo "  POST /echo/7 with a body  -> ${gateway_echo:-<no answer>}"
gateway_client="$(gateway_call /whoami appgen-native-key '' '203.0.113.9')"
echo "  GET /whoami via a proxy   -> ${gateway_client:-<no answer>}"

gateway_rc=0
case "$gateway_health" in
    200*'"ok"'*) ;;
    *) echo "  the gateway must answer its own declared route"; gateway_rc=1 ;;
esac
case "$gateway_nokey" in
    401*) ;;
    *) echo "  a caller with no API key must be refused before the handler"; gateway_rc=1 ;;
esac
case "$gateway_echo" in
    200*'"7"'*'"hi"'*) ;;
    *) echo "  a captured :id and a JSON body must both reach the handler"; gateway_rc=1 ;;
esac
# The caller arrives from 127.0.0.1, which this fixture's network.inbound names as a
# trusted proxy, so the address it forwards is the one the framework resolves. Without the
# list reaching the generated main the answer would be 127.0.0.1: the right answer for a
# surface that believes nobody, and the wrong one here.
case "$gateway_client" in
    200*'"203.0.113.9"'*) ;;
    *) echo "  a trusted proxy's forwarded address must be what the handler is handed"
       gateway_rc=1 ;;
esac
# The outbound half, from the same run. The entity's own file calls two URLs and only one of
# them is under the single prefix network.outbound names.
if ! grep -q "refused:.*network.outbound" "$WORK/gateway-gw.log"; then
    echo "  a URL outside network.outbound must be refused by Http, naming the allowlist"
    gateway_rc=1
fi
if grep -q "allowed:.*network.outbound" "$WORK/gateway-gw.log"; then
    echo "  a URL under the allowed prefix must not be refused by the allowlist"
    gateway_rc=1
fi
if [ "$gateway_rc" -ne 0 ]; then
    sed 's/^/  /' "$WORK/gateway-gw.log"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
cleanup_gateway
echo "  gateway       : OK (serves the routes its own QML declared, refuses an unkeyed"
echo "                  caller before the handler, and calls only what it is allowed to)"

echo "== [8/8] A monitor: two halves of one entity, and a console that outlives the topology =="
# The monitor is the one entity that is a mesh owner and a browser-facing server at once, and
# the only one whose Sources are generated from contracts no project file declares. Nothing
# but a build says whether that assembles. The console client compiles the framework's own
# Console.syn at the replica role, the monitor compiles both at the source role from
# SynQtMonitor, and its generated main has to find both registrations and link an HTTP
# server, a QML engine and a SQLite store into one binary.
MONITORED="$WORK/monitored"
cp -r "$REPO_ROOT/tests/appgen-native/monitored" "$MONITORED"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$MONITORED" "$REPO_ROOT" <<'MONPY'
import sys, yaml
from pathlib import Path
from synqt import appgen, check

app, repo = Path(sys.argv[1]), sys.argv[2]
ok, messages = check.check_project(app)
for message in messages:
    print("  synqt check:", message)
if not ok:
    raise SystemExit("the monitored fixture does not pass synqt check")
config = yaml.safe_load((app / "synqt.yaml").read_text())
print("  appgen wrote:", ", ".join(appgen.generate(app, config, synqt_root=repo)))
MONPY

cmake -S "$MONITORED" -B "$MONITORED/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$MONITORED/build"

monitored_rc=0
for entity in edge ops ops-console; do
    assert_native_exe "$MONITORED/build/$entity" "$entity" || monitored_rc=1
done
if [ "$monitored_rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
echo "  monitored     : OK (the monitor hosts its mesh point and serves its console from"
echo "                  one binary, and the console compiles against a contract no"
echo "                  project file declares)"

echo "APPGEN-NATIVE GATE: GO (appgen output compiles and links for every entity, a"
echo "                       generated client resolves every declared route to its view,"
echo "                       a promoted identity signs in from the auth entity, a"
echo "                       gateway serves the surface its network: block opened, and a"
echo "                       monitor assembles both of its halves into one binary)"
