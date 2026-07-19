#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Is Qt Quick 3D Physics usable on WebAssembly? The spec audit recorded "does not work on
# WASM"; this checks that against the pinned 6.11.1 kit in two halves that together settle it:
#
#   WASM half; build the scene with the wasm_singlethread kit (proves the Quick3DPhysics
#                 plugin and the bundled PhysX archive that ship in the kit actually link) and
#                 load it in a browser (proves Quick3D brings up its RHI over WebGL and the
#                 PhysX-backed scene instantiates and the event loop starts).
#   native half; build and run the identical QML + C++ on the desktop kit, where the render
#                 loop is real GL and not requestAnimationFrame-gated, and assert the box
#                 falls under gravity and rests on the static plane (PhysX steps and resolves
#                 the contact). This pins the correctness the WASM frame loop cannot sustain
#                 under headless automation.
#
# Conclusion when both pass: Quick3D Physics builds, loads, and boots on WASM, and the scene
# it runs is provably correct; the blanket "does not work on WASM" is too strong.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.11.1/wasm_singlethread}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HERE="$REPO_ROOT/tests/wasm-quick3dphysics"
cd "$REPO_ROOT"

echo "== [1/4] Build the scene (WASM single-threaded: Quick3D + bundled PhysX) =="
"$QT_WASM/bin/qt-cmake" -S tests/wasm-quick3dphysics -B build/q3dphys-wasm -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/q3dphys-wasm

echo "== [2/4] Build the same scene (native desktop kit) =="
cmake -S tests/wasm-quick3dphysics -B build/q3dphys-desktop -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/q3dphys-desktop

echo "== [3/4] WASM: load + boot in a browser =="
cd "$HERE/verify"
npm install --no-audit --no-fund
npx --yes playwright install chromium
PHYS_HEADLESS=1 node verify-phys.mjs

echo "== [4/4] Native: assert the box falls under gravity and rests on the plane =="
# Rendered offscreen with Mesa's software rasteriser, with no display and no X server. The
# note this replaces said Quick3D "needs a real GL context, so this cannot run on the
# offscreen platform" and reached for Xvfb instead; measured against the pinned 6.11.1 kit
# that is simply not true; the offscreen platform brings up the RHI through surfaceless
# EGL and prints the identical startY/minY/finalY. The Xvfb route cost a day to disprove:
# xvfb-run requires xauth, funnels the X server's own errors into a temp file it deletes on
# the way out, and so failed on the CI runner in under a second having emitted nothing at
# all. This path has no X server, no xauth and no display in it to go wrong, and it is the
# same one on a developer's machine as on a runner; the previous arrangement ran the two
# differently, which is the arrangement that let CI break without anyone's desktop noticing.
#
# LIBGL_ALWAYS_SOFTWARE is load-bearing, not belt-and-braces: without it, an offscreen run
# on a machine with no display hangs indefinitely rather than falling back (measured).
phys_log="$REPO_ROOT/build/q3dphys-desktop/native-run.log"
# tee, because the evidence is the point. The pipeline used to end in `grep | head -1`, which
# discards every line that is not the one being looked for, including the reason there is no
# such line. `head -1` is still what stops the run: the scene never exits on its own (its timer
# stops, the event loop does not), so closing the pipe after the line we came for is what ends
# it, and `timeout` is the backstop for the case where that line never comes.
OUT="$(timeout 90 env QT_QPA_PLATFORM=offscreen LIBGL_ALWAYS_SOFTWARE=1 \
    "$REPO_ROOT/build/q3dphys-desktop/quick3dphys-wasm" 2>&1 \
    | tee "$phys_log" | grep -E 'PHYS done' | head -1 || true)"
echo "  $OUT"
if [ -z "$OUT" ]; then
    echo "  --- the native run said this instead (tail of $phys_log): ---"
    tail -20 "$phys_log" | sed 's/^/  | /'
    echo "  ------------------------------------------------------------"
fi
# PHYS done startY=200.00 minY=-50.00 finalY=-50.00  -> fell far below start and settled above floor.
python3 - "$OUT" <<'PY'
import re, sys
line = sys.argv[1]
m = re.search(r"startY=([\-0-9.]+) minY=([\-0-9.]+) finalY=([\-0-9.]+)", line)
if not m:
    print("  native FAIL: no 'PHYS done' line (the run's own output is above)"); sys.exit(1)
startY, minY, finalY = map(float, m.groups())
fell = minY <= startY - 50
rested = finalY > -100 and abs(finalY - minY) < 25
print(f"  fell under gravity : {'PASS' if fell else 'FAIL'} (startY={startY} minY={minY})")
print(f"  rested on plane    : {'PASS' if rested else 'FAIL'} (finalY={finalY}, floor=-100)")
sys.exit(0 if fell and rested else 1)
PY
echo "PHYS GATE: GO (Quick3D Physics builds+loads on WASM; simulation proven on the native reference)"
