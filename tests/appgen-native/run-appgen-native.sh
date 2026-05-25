#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build what the app generator emits, on the native host kit, for the gavel topology plus
# the fixtures below. `routed/` and `promoted/` also run what they build.
#
# Usage: tests/appgen-native/run-appgen-native.sh

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=../lib/native-binary.sh
. "$REPO_ROOT/tests/lib/native-binary.sh"

if [ ! -x "$QT_HOST/bin/qmake" ] && [ ! -d "$QT_HOST/lib/cmake" ]; then
    echo "error: native host kit not found at $QT_HOST" >&2
    exit 1
fi

# Point the tooling at the same kit, so `synqt check` finds qmllint.
export QTDIR="$QT_HOST"

WORK="$REPO_ROOT/build/appgen-native"
SRC="$WORK/gavel"
echo "== [1/8] Materialize the gavel topology and run appgen over it =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$REPO_ROOT/examples/gavel" "$SRC"
# Copy the tracked sources only, never a developer's build state.
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
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
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
# Every route's view, and everything it reaches, must be in the client's QML module.
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
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$ROUTED/build" --target app

routed_exe="$(native_exe_path "$ROUTED/build/app")"
if [ -z "$routed_exe" ]; then
    echo "  routed client : MISSING"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
# The fixture's Main.qml walks the route table and reports each resolution.
routed_log="$WORK/routed-run.log"
QT_QPA_PLATFORM=offscreen "$routed_exe" >"$routed_log" 2>&1 || true
sed 's/^/  /' "$routed_log"
# Home uses Panel.qml and the Theme.qml singleton, which no route names. /help is a view
# in a subdirectory.
for expected in "SYNQT-ROUTE path=/ status=Ready view=Home(panel,dark)" \
                "SYNQT-ROUTE path=/about status=Ready view=About" \
                "SYNQT-ROUTE path=/help status=Ready view=Help"; do
    if ! grep -qF "$expected" "$routed_log"; then
        echo "  expected the routed client to report: $expected"
        echo "APPGEN-NATIVE GATE: NO-GO"
        exit 1
    fi
done
# A clean run prints those three lines and no QML diagnostic.
if grep -nE '\.qml:[0-9]+:' "$routed_log"; then
    echo "  the routed client logged a QML diagnostic; a clean run reports only its routes"
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
echo "  routed client : OK (every route resolved Ready, each to the view it names)"

echo "== [5/8] Promoted identity: one line moves the OAuth engine off the edge =="
# `identity.provider_entity: auth`: run the pair and ask the edge for a login it cannot
# answer alone.
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
# dev_tools=True: the development sign-in exists only in a `synqt dev` tree.
print("  appgen wrote:", ", ".join(appgen.generate(app, config, synqt_root=repo,
                                                   dev_tools=True)))
mesh.init(app)
print("  " + mesh.cert_all(app, ["edge", "auth"]).replace("\n", "\n  "))
print("  topology:", ", ".join(topologywriter.write(app, config)))
PY

# Out of tree: topologywriter owns build/<entity>/.
cmake -S "$PROMOTED" -B "$PROMOTED/out" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_ROOT="$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSYNQT_DEV_TOOLS=ON
cmake --build "$PROMOTED/out"

rc=0
for entity in app edge auth; do
    assert_native_exe "$PROMOTED/out/$entity" "$entity" || rc=1
done
if [ "$rc" -ne 0 ]; then
    echo "APPGEN-NATIVE GATE: NO-GO"
    exit 1
fi
# Native paths for Python, which cannot resolve MSYS paths.
promoted_web="$(native_exe_path "$PROMOTED/out/edge")"
promoted_auth="$(native_exe_path "$PROMOTED/out/auth")"

mkdir -p "$PROMOTED/build/client"
printf '<!doctype html>\n' > "$PROMOTED/build/client/index.html"
# Every `kill` swallows its failure: the script runs under `set -e`.
cleanup_promoted() {
    kill "${auth_pid:-}" "${edge_pid:-}" 2>/dev/null || true
}
trap cleanup_promoted EXIT
# The secret reaches only the entity that runs the token exchange.
export GITHUB_CLIENT_SECRET="appgen-native-not-a-real-secret"
export QT_QPA_PLATFORM=offscreen
# `exec`, so $! is the entity itself. --dev on the auth entity too, as `synqt dev` passes it;
# without it the edge answers the login route with 403.
(cd "$PROMOTED" && exec ./out/auth --dev >"$WORK/promoted-auth.log" 2>&1) &
auth_pid=$!
sleep 2
# --dev only for the plaintext loopback listener. The QML directory is the generated/
# mirror, as under `synqt dev`.
(cd "$PROMOTED" && exec ./out/edge --bundle build/client --qml-dir generated \
    --port 18443 --dev >"$WORK/promoted-web.log" 2>&1) &
edge_pid=$!

# Wait until both mesh links are up and both Replicas adopted.
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

# The edge binary must not contain the client id or the authorize URL. Both encodings
# (UTF-16 and narrow) are searched as raw bytes, since `strings -el` is GNU-only. An
# unreadable file is an error, never an absence.
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
# The auth binary must contain both.
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

# The development sign-in, served by the edge and dialled by the auth entity. `synqt
# serve` passes no --dev, so a deployed tree answers 403.
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

# Two people are configured, so the stub asks which.
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
# A front: the edge has no server file for that point, and its relay resolves against a
# contract its binary knows only by name. Building it is the check.
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
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
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
# The generated main must set `Api` before the entity singleton exists and listen after.
# Only running it shows that.
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
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
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

# A whole URL, since MSYS rewrites a value that looks like a POSIX path.
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
    # Say whether the port refused or the entity never replied.
    print("- %s: %s" % (type(error).__name__, error))
PY
}

# Wait for the entity to say it is listening.
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
# 127.0.0.1 is a trusted proxy here, so the forwarded address is the answer.
case "$gateway_client" in
    200*'"203.0.113.9"'*) ;;
    *) echo "  a trusted proxy's forwarded address must be what the handler is handed"
       gateway_rc=1 ;;
esac
# The entity calls two URLs; only one is under network.outbound.
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
# The monitor: a mesh owner and a browser-facing server whose Sources come from framework
# contracts. Building it is the check.
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
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
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
