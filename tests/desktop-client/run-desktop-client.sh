#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build, install and boot the native desktop client through the real tooling
# (`presets.write` + `build.compile_incremental(client="desktop")`) on the gavel topology,
# with the client targeting wasm and desktop. Asserts a native executable, the baked-in
# edge URL, and a boot that does not crash (offscreen, edge unreachable).
#
# Usage: tests/desktop-client/run-desktop-client.sh

set -euo pipefail

# No default: the kit directory name differs per platform.
case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.12.0/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.12.0/gcc_64 ;;
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

# Point the tooling at the same kit through QTDIR.
export QTDIR="$QT_HOST"

EDGE_URL="wss://desktop-edge.synqt.test:9443/sync"
WORK="$REPO_ROOT/build/desktop-client"
SRC="$WORK/gavel"

echo "== [1/4] Materialize gavel, mark the client a desktop target, run the tooling =="
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$REPO_ROOT/examples/gavel" "$SRC"
# Copy the tracked sources only, never a developer's build state.
rm -rf "$SRC/build" "$SRC/CMakeUserPresets.json"

PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$SRC" "$REPO_ROOT" "$EDGE_URL" <<'PY'
import sys
from pathlib import Path

import yaml

from synqt import appgen, appmodel, build, presets

app, repo, edge_url = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
config = yaml.safe_load((app / "synqt.yaml").read_text())

# Target wasm and desktop, with an edge URL to bake in.
for entity in config["entities"]:
    if appmodel.is_client(entity):
        entity["targets"] = ["wasm", "desktop"]
config.setdefault("build", {}).setdefault("desktop", {})["edge_url"] = edge_url
(app / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))

# The presets, then the real incremental build path.
presets.write(app, config)
note, host_targets, client_targets = build.compile_incremental(app, config, client="desktop")
print("  host_targets  :", ", ".join(host_targets))
print("  client_targets:", ", ".join(client_targets))
print("  compile note  :", note)
if note.startswith("error") or note.startswith("note:"):
    sys.exit("desktop client build did not compile: " + note)
PY

# The client entity's name is the target and installed file name.
CLIENT="$(PYTHONPATH="$REPO_ROOT/tools/synqt" python3 -c \
    'import sys, yaml; from synqt import appmodel;
c = yaml.safe_load(open(sys.argv[1] + "/synqt.yaml"));
print(appmodel.client_entity(c)["name"])' "$SRC")"

echo "== [2/4] Assert the desktop client compiled and installed =="
# The deploy folder is per platform; ask the tooling.
PLATFORM="$(PYTHONPATH="$REPO_ROOT/tools/synqt" python3 -c \
    'from synqt import build; print(build.desktop_platform())')"
# compile_incremental builds the development tree; ask for its directory.
HOST_DIR="$SRC/$(PYTHONPATH="$REPO_ROOT/tools/synqt" python3 -c \
    "from synqt import profiles; print(profiles.build_dir('host', 'debug', dev_tools=True))")"
HOST_BIN="$(native_exe_path "$HOST_DIR/$CLIENT")"
INSTALLED="$(native_exe_path "$SRC/build/client-desktop/$PLATFORM/$CLIENT")"
rc=0
assert_native_exe "$HOST_DIR/$CLIENT" "compiled " || rc=1
assert_native_exe "$SRC/build/client-desktop/$PLATFORM/$CLIENT" "installed" || rc=1

# Steps 3 and 4 both need the binary.
if [ "$rc" -ne 0 ]; then
    echo "DESKTOP-CLIENT GATE: NO-GO (the client did not compile or install)"
    exit 1
fi

echo "== [2b/4] Deploy on a copy, and assert the result carries its own Qt =="
# Deploy a copy: `synqt build` does not deploy, so step 4 boots the undeployed binary. Runs
# on every platform, since the Linux portable layout is SynQt's own code.
PROBE="$WORK/deploy-probe"
rm -rf "$PROBE"
mkdir -p "$PROBE"

# Through the tooling's deploy module, the path `synqt build --deploy` takes.
deploy_probe() {
    PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - \
        "$SRC" "$PROBE" "$QT_HOST" "$PLATFORM" "$CLIENT" <<'PY'
import sys
from pathlib import Path

from synqt import deploy

root, out, kit, platform = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
# --unsigned: a build machine has no signing identity.
deploy.check_signing_choice(platform, None, True)
print("   ", deploy.deploy_client(root, sys.argv[5], out, {"host_qt": kit},
                                  platform, sign=None))
PY
}

if [ "$PLATFORM" = "macos" ]; then
    # macdeployqt accepts only a .app bundle.
    APP="$SRC/build/client-desktop/macos/$CLIENT.app"
    if [ -d "$APP" ] && [ -f "$APP/Contents/Info.plist" ]; then
        BUNDLE_ID="$(defaults read "$APP/Contents/Info" CFBundleIdentifier 2>/dev/null || echo "")"
        echo "  bundle   : OK (.app with Info.plist, CFBundleIdentifier=$BUNDLE_ID)"
    else
        echo "  bundle   : FAIL (no .app bundle at $APP; macdeployqt cannot be run)"; rc=1
    fi

    # Record the LC_RPATH into the build kit before deploying. `otool -l`, since `otool -L`
    # lists only @rpath entries.
    CLIENT_BIN="$APP/Contents/MacOS/$CLIENT"
    before="$(otool -l "$CLIENT_BIN" 2>/dev/null | grep -c "$QT_HOST" || true)"
    echo "  pre-deploy: $before LC_RPATH reference(s) into the build kit ($QT_HOST)"

    if [ "$rc" -eq 0 ]; then
        # A deployed bundle ships only the cocoa plugin, so it cannot boot offscreen.
        cp -R "$APP" "$PROBE/$CLIENT.app"
        APP="$PROBE/$CLIENT.app"
        CLIENT_BIN="$APP/Contents/MacOS/$CLIENT"
        deploy_probe || rc=1
        # Self-contained: the frameworks are in the bundle and a bundle-relative rpath finds them.
        own_rpath="$(otool -l "$CLIENT_BIN" 2>/dev/null \
            | grep -c "@executable_path/../Frameworks" || true)"
        if [ -d "$APP/Contents/Frameworks/QtCore.framework" ] && [ "$own_rpath" -gt 0 ]; then
            kit_left="$(otool -l "$CLIENT_BIN" 2>/dev/null | grep -c "$QT_HOST" || true)"
            echo "  deployed : OK (Qt travels in the bundle, bundle-relative rpath present;" \
                 "$kit_left kit rpath left)"
            # Running the deployed copy needs a real display, which a macOS runner lacks.
        else
            echo "  deployed : FAIL (QtCore in bundle=$([ -d "$APP/Contents/Frameworks/QtCore.framework" ] && echo yes || echo no)," \
                 "bundle-relative rpath=$own_rpath)"
            rc=1
        fi
    fi
elif [ "$PLATFORM" = "windows" ]; then
    cp "$INSTALLED" "$PROBE/$(basename "$INSTALLED")"
    deploy_probe || rc=1
    # windeployqt puts the DLLs beside the exe and the plugin directories under it.
    windows_missing=""
    for needed in Qt6Core.dll Qt6Qml.dll Qt6Quick.dll platforms/qwindows.dll; do
        [ -e "$PROBE/$needed" ] || windows_missing="$windows_missing $needed"
    done
    if [ -n "$windows_missing" ]; then
        echo "  deployed : FAIL (missing from the deployed tree:$windows_missing)"; rc=1
    else
        echo "  deployed : OK (Qt DLLs and the Windows platform plugin beside the exe)"
    fi
else
    cp "$INSTALLED" "$PROBE/$(basename "$INSTALLED")"
    deploy_probe || rc=1

    if [ "$rc" -eq 0 ]; then
        # Named explicitly: both are loaded only at run time.
        linux_missing=""
        for needed in lib/libQt6XcbQpa.so.6 lib/libQt6QuickControls2Impl.so.6 \
                      plugins/platforms/libqxcb.so qml/QtQuick/Controls/Basic/qmldir \
                      "$CLIENT.sh"; do
            [ -e "$PROBE/$needed" ] || linux_missing="$linux_missing $needed"
        done
        # And nothing extra: no Qt Sql, no virtual keyboard.
        for absent in qml/QtQuick/VirtualKeyboard plugins/sqldrivers; do
            [ ! -e "$PROBE/$absent" ] || linux_missing="$linux_missing (unwanted:$absent)"
        done
        if [ -n "$linux_missing" ]; then
            echo "  layout   : FAIL$linux_missing"; rc=1
        else
            echo "  layout   : OK ($(du -sm "$PROBE" | cut -f1) MB: the modules it imports," \
                 "the plugins it can load, and the libraries all of that links)"
        fi
    fi

    if [ "$rc" -eq 0 ]; then
        # Every Qt library, QML module and plugin the running process maps must come from inside
        # the deployed tree (/proc/<pid>/maps).
        set +e
        SYNQT_PROBE="$PROBE" SYNQT_CLIENT="$CLIENT" python3 - <<'PY'
import os
import subprocess
import sys

probe = os.environ["SYNQT_PROBE"]
process = subprocess.Popen([os.path.join(probe, os.environ["SYNQT_CLIENT"] + ".sh")],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           env={**os.environ, "QT_QPA_PLATFORM": "offscreen"})
try:
    code = process.wait(timeout=4)
    sys.exit(f"the deployed client exited ({code}) instead of staying up; a library or a "
             "QML module it needs is not in the tree")
except subprocess.TimeoutExpired:
    pass  # still running at the deadline: it booted and stayed up

try:
    with open(f"/proc/{process.pid}/maps", encoding="utf-8") as handle:
        maps = handle.read()
finally:
    process.kill()
    process.wait()

outside = set()
for line in maps.splitlines():
    fields = line.split()
    if len(fields) < 6:
        continue  # an anonymous mapping has no pathname column
    path = fields[5]
    if not path.startswith("/"):
        continue
    interesting = (os.path.basename(path).startswith("libQt6")
                   or "/qml/" in path or "/plugins/" in path)
    if interesting and not path.startswith(probe):
        outside.add(path)
if outside:
    sys.exit("the running client mapped Qt from outside the deployed tree: "
             + ", ".join(sorted(outside)))
print("   every Qt library the running client mapped came from the deployed tree")
PY
        deployed_rc=$?
        set -e
        if [ "$deployed_rc" -eq 0 ]; then
            echo "  self-contained: OK (boots, and loads no Qt from the host)"
        else
            echo "  self-contained: FAIL (see above)"; rc=1
        fi
    fi
fi

echo "== [3/4] Assert build.desktop.edge_url was baked into the binary =="
# The URL is stored as UTF-16, so scan both encodings, in Python (no `strings` on Windows).
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
# A good boot is still running at the deadline, blocked reaching the edge; a crash exits
# fast. The deadline is enforced in Python (no `timeout` on Windows).
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
