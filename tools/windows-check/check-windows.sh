#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build a CMake source directory for the Windows MSVC ABI with clang-cl, lld-link, an `xwin
# splat` tree, the Windows Qt kit and a Linux host kit for the generators. Compiles and
# links only; see cmake/toolchains/windows-clang-cl.cmake. Provisioning is in README.md.
#
# Environment:
#   XWIN_DIR    the `xwin splat` output directory (required. Holds crt/ and sdk/)
#   QT_WIN      the Windows Qt kit (default: $HOME/Qt-win/6.12.0/msvc2022_64)
#   QT_HOST     the Linux host Qt kit whose generators run here
#               (default: /opt/Qt/6.12.0/gcc_64)
#   OPENSSL_WIN a Windows OpenSSL prefix, needed by anything linking SynQtService
#               (default: $HOME/.cache/synqt-openssl-win/Library; ignored if absent)
#   BUILD_TYPE  CMake build type (default: RelWithDebInfo)
#
#   ./check-windows.sh                 # build the QtCore probe
#   ./check-windows.sh <cmake-src-dir> # build any target directory

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

TOOLCHAIN="$REPO_ROOT/cmake/toolchains/windows-clang-cl.cmake"
QT_WIN="${QT_WIN:-$HOME/Qt-win/6.12.0/msvc2022_64}"
QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
OPENSSL_WIN="${OPENSSL_WIN:-$HOME/.cache/synqt-openssl-win/Library}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

SRC_DIR="${1:-$SCRIPT_DIR}"
# A stable, collision-free build directory name per source dir.
TAG="$(printf '%s' "$SRC_DIR" | tr -c 'A-Za-z0-9' '_')"
BUILD_DIR="$REPO_ROOT/build/win-check/$TAG"

fail() { echo "check-windows: $*" >&2; exit 2; }

command -v clang-cl >/dev/null 2>&1 || fail "clang-cl not on PATH (install clang/llvm)"
command -v lld-link >/dev/null 2>&1 || fail "lld-link not on PATH (install lld)"
[ -n "${XWIN_DIR:-}" ] || fail "set XWIN_DIR to the 'xwin splat' output directory"
[ -d "$XWIN_DIR/crt/include" ] || fail "XWIN_DIR='$XWIN_DIR' has no crt/include (run 'xwin splat')"
[ -d "$QT_WIN/lib/cmake/Qt6" ] || fail "QT_WIN='$QT_WIN' is not a Windows Qt kit (no lib/cmake/Qt6)"
[ -x "$QT_HOST/bin/qtpaths" ] || [ -x "$QT_HOST/bin/moc" ] \
    || fail "QT_HOST='$QT_HOST' is not a host Qt kit (no bin/moc)"

# Optional: only targets linking SynQtService need a Windows OpenSSL.
prefix_path="$QT_WIN"
openssl_args=()
if [ -d "$OPENSSL_WIN/include/openssl" ]; then
    prefix_path="$QT_WIN;$OPENSSL_WIN"
    openssl_args+=(-DOPENSSL_ROOT_DIR="$OPENSSL_WIN")
    openssl_note="$OPENSSL_WIN"
else
    openssl_note="(none; SynQtService targets will fail at find_package(OpenSSL))"
fi

echo "check-windows: source   = $SRC_DIR"
echo "check-windows: xwin      = $XWIN_DIR"
echo "check-windows: qt-win    = $QT_WIN"
echo "check-windows: qt-host   = $QT_HOST"
echo "check-windows: openssl   = $openssl_note"
echo "check-windows: build dir = $BUILD_DIR"
echo "check-windows: clang     = $(clang-cl --version | head -1)"

export XWIN_DIR

rm -rf "$BUILD_DIR"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DQT_HOST_PATH="$QT_HOST" \
    -DCMAKE_PREFIX_PATH="$prefix_path" \
    "${openssl_args[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" || fail "configure failed"

cmake --build "$BUILD_DIR" || fail "build failed"

echo "check-windows: BUILD OK"
# List the PE artifacts produced.
found=0
while IFS= read -r exe; do
    found=1
    echo "check-windows: artifact $(basename "$exe") -> $(file -b "$exe" 2>/dev/null || echo '?')"
done < <(find "$BUILD_DIR" -maxdepth 3 -name '*.exe' -not -path '*/CMakeFiles/*' 2>/dev/null)
[ "$found" -eq 1 ] || echo "check-windows: note: no .exe found (a library-only target is fine)"
