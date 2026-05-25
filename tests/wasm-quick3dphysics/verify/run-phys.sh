#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Qt Quick 3D Physics on WebAssembly, in two halves:
#
#   WASM half.   Build the scene with the wasm_singlethread kit and load it in a browser: the
#                plugin and PhysX link, Quick3D brings up its RHI over WebGL, the scene starts.
#   native half. Run the same QML and C++ on the desktop kit and assert the box falls under
#                gravity and rests on the plane.

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"
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
# Offscreen, with Mesa's software rasteriser and no display: the offscreen platform brings
# up the RHI through surfaceless EGL. LIBGL_ALWAYS_SOFTWARE is required, or a machine with
# no display hangs.
phys_log="$REPO_ROOT/build/q3dphys-desktop/native-run.log"
# tee keeps the evidence. The scene never exits, so `head -1` closing the pipe ends it,
# with `timeout` as the backstop.
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
