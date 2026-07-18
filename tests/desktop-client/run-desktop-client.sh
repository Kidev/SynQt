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

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
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
