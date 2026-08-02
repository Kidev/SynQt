#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Prove the NATIVE DESKTOP client target actually compiles, installs, and boots; the missing
# analogue of tests/appgen-native, which builds the service/edge mains but never drove the
# `synqt build --client desktop` tooling path end to end. The desktop client shares the client
# QML and SynClient runtime with the WASM build; only two things differ (docs/desktop.md): it
# terminates its own TLS with QSslSocket, and it reads the edge URL from build.desktop.edge_url
# instead of the served page. This fixture exercises exactly that path:
#
#   1. materialize the three-entity gavel topology and mark the client `targets: [wasm, desktop]`
#      with a distinctive build.desktop.edge_url;
#   2. run the real tooling (presets.write + build.compile_incremental(client="desktop")), which
#      generates the client main/CMake, configures the host preset, compiles the client on the
#      native kit, and installs it under build/client-desktop/linux/;
#   3. assert the installed binary is a native executable, that the configured edge URL is baked
#      into it (SYNQT_EDGE_URL; the desktop client has no serving origin to read it from), and
#      that it boots the QML engine + SynClient without crashing (offscreen, edge unreachable).
#
# The string-level unit tests assert the generated CMake/main text; only a real build catches a
# missing link library, a CMake collision, or (as this fixture first found) build.desktop.
# edge_url never being passed to the compile. Needs the pinned host kit (/opt/Qt/6.11.1/gcc_64).
#
# Usage: tests/desktop-client/run-desktop-client.sh

set -euo pipefail

# The kit directory is named for the host, not the target, so a single Linux default makes this
# script fail on macOS with "native host kit not found" for a kit that is installed and correct.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.11.1/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.11.1/gcc_64 ;;
esac

QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=../lib/native-binary.sh
. "$REPO_ROOT/tests/lib/native-binary.sh"

if [ ! -d "$QT_HOST/lib/cmake" ]; then
    echo "error: native host kit not found at $QT_HOST" >&2
    exit 1
fi

# Point the tooling's resolver at the same kit this script was told to use. The resolver
# searches the project toolchain dir, the system prefixes, and QTDIR (toolchain.py); on CI
# the kit is at none of the first two, so without this the real `synqt build` path below
# resolves no host Qt, reports "toolchain incomplete", and skips the compile this fixture
# exists to perform. QTDIR is the product's own documented escape hatch, so using it here
# keeps the fixture on a supported path rather than reaching past the tooling.
export QTDIR="$QT_HOST"

EDGE_URL="wss://desktop-edge.synqt.test:9443/sync"
WORK="$REPO_ROOT/build/desktop-client"
SRC="$WORK/gavel"

echo "== [1/4] Materialize gavel, mark the client a desktop target, run the tooling =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$REPO_ROOT/examples/gavel" "$SRC"
# The example is a working directory for whoever has run `synqt build` in it, and those
# leftovers are gitignored, so a fresh clone never has them and a developer's checkout
# does. Copying them in points the `host` preset at a CMakeCache built for another source
# tree, which CMake refuses. Take the tracked sources, not the state.
rm -rf "$SRC/build" "$SRC/CMakeUserPresets.json"

PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$SRC" "$REPO_ROOT" "$EDGE_URL" <<'PY'
import sys
from pathlib import Path

import yaml

from synqt import appgen, build, presets

app, repo, edge_url = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
config = yaml.safe_load((app / "synqt.yaml").read_text())

# Turn the WASM-only client into a dual wasm+desktop target and give the desktop build an edge
# URL to bake in. This is the one config change docs/desktop.md says a desktop client needs.
for entity in config["entities"]:
    if entity.get("kind") == "client":
        entity["targets"] = ["wasm", "desktop"]
config.setdefault("build", {}).setdefault("desktop", {})["edge_url"] = edge_url
(app / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

# Generate the CMakePresets (host preset -> build/host) the tooling configures against, then run
# the real incremental build path for the desktop client. appgen + topologywriter run inside it.
presets.write(app, config)
note, host_targets, client_targets = build.compile_incremental(app, config, client="desktop")
print("  host_targets  :", ", ".join(host_targets))
print("  client_targets:", ", ".join(client_targets))
print("  compile note  :", note)
if note.startswith("error") or note.startswith("note:"):
    sys.exit("desktop client build did not compile: " + note)
PY

echo "== [2/4] Assert the desktop client compiled and installed =="
# The deploy folder is per platform (docs/desktop.md names windows/, macos/, linux/), and the
# tooling picks it from the host, so ask the tooling rather than hard-code one of the three.
PLATFORM="$(PYTHONPATH="$REPO_ROOT/tools/synqt" python3 -c \
    'from synqt import build; print(build.desktop_platform())')"
HOST_BIN="$(native_exe_path "$SRC/build/host/client")"
INSTALLED="$(native_exe_path "$SRC/build/client-desktop/$PLATFORM/client")"
rc=0
assert_native_exe "$SRC/build/host/client" "compiled " || rc=1
assert_native_exe "$SRC/build/client-desktop/$PLATFORM/client" "installed" || rc=1

# Steps 3 and 4 both read the built binary, so stop here rather than report confusing
# follow-on failures for a binary that does not exist.
if [ "$rc" -ne 0 ]; then
    echo "DESKTOP-CLIENT GATE: NO-GO (the client did not compile or install)"
    exit 1
fi

if [ "$PLATFORM" = "macos" ]; then
    echo "== [2b/4] macOS: assert an .app bundle, and that macdeployqt can finish it =="
    # A bare Mach-O is not an app on macOS, and macdeployqt accepts nothing else, so a build
    # that emits one makes the deploy step docs/desktop.md hands to the developer impossible
    # to perform without rewriting the generated CMake. That is what this asserts: not that
    # the build deploys (it deliberately does not), but that what it produces is something the
    # documented command can be run against.
    APP="$SRC/build/client-desktop/macos/client.app"
    if [ -d "$APP" ] && [ -f "$APP/Contents/Info.plist" ]; then
        BUNDLE_ID="$(defaults read "$APP/Contents/Info" CFBundleIdentifier 2>/dev/null || echo "")"
        echo "  bundle   : OK (.app with Info.plist, CFBundleIdentifier=$BUNDLE_ID)"
    else
        echo "  bundle   : FAIL (no .app bundle at $APP; macdeployqt cannot be run)"; rc=1
    fi

    # Before deploying, Qt is reached through an LC_RPATH pointing into the build kit, so the
    # app runs only on a machine that has that kit at that path. Recorded so the after-state
    # below reads as a change rather than as an assertion about an unknown starting point.
    #
    # Read with `otool -l`, not `otool -L`: the kit path is an LC_RPATH load command, while the
    # Qt entries `otool -L` prints are all `@rpath/...` and name no kit at all. Grepping the -L
    # output for the kit therefore matches nothing whether or not the bundle is deployed, which
    # is a check that passes for the broken case as readily as for the fixed one.
    CLIENT_BIN="$APP/Contents/MacOS/client"
    before="$(otool -l "$CLIENT_BIN" 2>/dev/null | grep -c "$QT_HOST" || true)"
    echo "  pre-deploy: $before LC_RPATH reference(s) into the build kit ($QT_HOST)"

    if [ "$rc" -eq 0 ]; then
        # Deployed on a COPY, for two reasons. `synqt build` does not deploy (docs/desktop.md),
        # so the artifact it installs is the undeployed one and that is what step 4 must boot;
        # deploying in place would mean the fixture asserts the boot of something the build never
        # produces. And macdeployqt ships only the platform plugin a released app needs (cocoa),
        # so a deployed bundle cannot be booted with QT_QPA_PLATFORM=offscreen at all -- it
        # aborts with "Could not find the Qt platform plugin", which is correct behaviour for a
        # deployed app and was, briefly, this fixture reporting a crash that was its own doing.
        PROBE="$WORK/deploy-probe"
        rm -rf "$PROBE"
        mkdir -p "$PROBE"
        cp -R "$APP" "$PROBE/client.app"
        APP="$PROBE/client.app"
        CLIENT_BIN="$APP/Contents/MacOS/client"
        # Through the tooling's own deploy module, not by calling macdeployqt here: the point
        # is to test the code path `synqt build --deploy` takes, and a fixture that ran the
        # command itself would keep passing after that path broke.
        PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$SRC" "$PROBE" "$QT_HOST" <<'PY'
import sys
from pathlib import Path

from synqt import deploy

root, out, kit = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
# --unsigned is the fixture's choice, stated the way the CLI makes a caller state it. There is
# no signing identity on a build machine, and a fixture that signed would be testing the
# developer's keychain rather than SynQt.
deploy.check_signing_choice("macos", None, True)
print("   ", deploy.deploy_client(root, "client", out, {"host_qt": kit}, "macos", sign=None))
PY
        # Self-contained is asserted structurally rather than by the kit rpath disappearing:
        # whether macdeployqt strips the original LC_RPATH or merely prepends its own has
        # varied, and an app that carries its Qt and looks in its own bundle first is
        # correct either way. So: the frameworks travel inside the bundle, and the binary
        # has a bundle-relative rpath to find them by.
        own_rpath="$(otool -l "$CLIENT_BIN" 2>/dev/null \
            | grep -c "@executable_path/../Frameworks" || true)"
        if [ -d "$APP/Contents/Frameworks/QtCore.framework" ] && [ "$own_rpath" -gt 0 ]; then
            kit_left="$(otool -l "$CLIENT_BIN" 2>/dev/null | grep -c "$QT_HOST" || true)"
            echo "  deployed : OK (Qt travels in the bundle, bundle-relative rpath present;" \
                 "$kit_left kit rpath left)"
            # Not asserted: that the deployed copy runs. It ships only the cocoa plugin, so it
            # needs a real display, which a macOS CI runner does not have. What is asserted is
            # that the hand-off docs/desktop.md documents can be performed and produces an app
            # that carries its own Qt; running it is the developer's own signing-and-ship path.
        else
            echo "  deployed : FAIL (QtCore in bundle=$([ -d "$APP/Contents/Frameworks/QtCore.framework" ] && echo yes || echo no)," \
                 "bundle-relative rpath=$own_rpath)"
            rc=1
        fi
    fi
fi

echo "== [3/4] Assert build.desktop.edge_url was baked into the binary =="
# QStringLiteral(SYNQT_EDGE_URL) stores the URL as UTF-16, so scan both the ASCII and the
# 16-bit-little-endian encodings. Done in Python, not with `strings`: binutils is not part of
# a Git-for-Windows install, so `strings` is simply absent on the Windows runner, and a scan
# that silently finds nothing there would read as "the URL was never baked in".
if SYNQT_BIN="$HOST_BIN" SYNQT_NEEDLE="desktop-edge.synqt.test:9443" python3 - <<'PY'
import os, sys
data = open(os.environ["SYNQT_BIN"], "rb").read()
needle = os.environ["SYNQT_NEEDLE"]
found = needle.encode("ascii") in data or needle.encode("utf-16-le") in data
sys.exit(0 if found else 1)
PY
then
    echo "  edge URL : OK (SYNQT_EDGE_URL = $EDGE_URL)"
else
    echo "  edge URL : MISSING; build.desktop.edge_url was not passed to the compile"; rc=1
fi

echo "== [4/4] Boot the desktop client headless (offscreen); the edge is unreachable =="
# A successful boot loads the QML engine + SynClient and then blocks in app.exec() trying to
# reach the (unresolvable) edge, so it is still alive when the deadline passes. A crash or a
# failed QML load (main returns -1) exits fast instead.
#
# The deadline is enforced in Python rather than with `timeout`: coreutils' timeout is not part
# of a Git-for-Windows install. This also states the pass condition directly ("still running"),
# where the old form asserted the exit code 124 that only GNU timeout produces.
set +e
SYNQT_BIN="$INSTALLED" python3 - <<'PY'
import os, subprocess, sys

process = subprocess.Popen([os.environ["SYNQT_BIN"]],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           env={**os.environ, "QT_QPA_PLATFORM": "offscreen"})
try:
    code = process.wait(timeout=4)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait()
    sys.exit(0)  # still running at the deadline: it booted and stayed up
sys.exit(f"exited {code} before the deadline")
PY
boot_rc=$?
set -e
if [ "$boot_rc" -eq 0 ]; then
    echo "  boots    : OK (engine + SynClient came up and kept running)"
else
    echo "  boots    : FAIL (crash or empty QML root; see above)"; rc=1
fi

echo
if [ "$rc" -ne 0 ]; then
    echo "DESKTOP-CLIENT GATE: NO-GO"
    exit 1
fi
echo "DESKTOP-CLIENT GATE: GO (desktop client compiles, installs, bakes its edge URL, and boots)"
