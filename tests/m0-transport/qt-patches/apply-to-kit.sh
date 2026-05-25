#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Apply the QEventDispatcherWasm posted-event patch to an installed Qt WASM kit, or revert
# it (README.md). Recompiles one translation unit against the kit's headers and swaps the
# object into libQt6Core.a; `--verify` checks its symbols match the stock object. The stock
# archive stays as libQt6Core.a.stock.

set -euo pipefail

QT_WASM="${QT_WASM:-/opt/Qt/6.12.0/wasm_singlethread}"
QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
QT_SRC="${QT_SRC:-/opt/Qt/6.12.0/Src/qtbase}"

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
PATCH="$HERE/0001-wasm-send-posted-events-from-the-native-timer.patch"
RELPATH="src/corelib/kernel/qeventdispatcher_wasm.cpp"
MEMBER="qeventdispatcher_wasm.cpp.o"
ARCHIVE="$QT_WASM/lib/libQt6Core.a"
STOCK="$ARCHIVE.stock"

action="${1:-apply}"

usage() {
    echo "usage: $(basename "$0") [apply|revert|status|verify]"
    echo
    echo "  QT_WASM  Qt WASM kit to patch     (default $QT_WASM)"
    echo "  QT_HOST  host kit, for moc        (default $QT_HOST)"
    echo "  QT_SRC   qtbase sources           (default $QT_SRC)"
}

status() {
    if [ -f "$STOCK" ]; then
        echo "patched: $ARCHIVE"
        echo "stock kept at: $STOCK"
    else
        echo "stock: $ARCHIVE"
    fi
}

case "$action" in
-h | --help | help)
    usage
    exit 0
    ;;
status)
    status
    exit 0
    ;;
revert)
    if [ ! -f "$STOCK" ]; then
        echo "nothing to revert: $STOCK does not exist, so the kit is already stock." >&2
        exit 1
    fi
    cp -a "$STOCK" "$ARCHIVE"
    rm -f "$STOCK"
    echo "reverted $ARCHIVE to the archive Qt shipped."
    echo "Relink anything already built against it (rm the .wasm and build again)."
    exit 0
    ;;
apply | verify) ;;
*)
    usage >&2
    exit 2
    ;;
esac

for path in "$QT_WASM/lib" "$QT_HOST/libexec/moc" "$QT_SRC/$RELPATH"; do
    if [ ! -e "$path" ]; then
        echo "error: $path not found. Set QT_WASM, QT_HOST and QT_SRC to your install." >&2
        exit 1
    fi
done

# shellcheck source=../../lib/emsdk.sh
. "$REPO_ROOT/tests/lib/emsdk.sh"
synqt_activate_emsdk

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cp "$QT_SRC/$RELPATH" "$WORK/qeventdispatcher_wasm.cpp"
patch -s -d "$WORK" -i "$PATCH" qeventdispatcher_wasm.cpp

PRIVATE="$QT_WASM/include/QtCore/6.12.0/QtCore/private"
INCLUDES=(
    "-I$QT_WASM/include"
    "-I$QT_WASM/include/QtCore"
    "-I$QT_WASM/include/QtCore/6.12.0"
    "-I$QT_WASM/include/QtCore/6.12.0/QtCore"
    "-I$PRIVATE"
    "-I$QT_WASM/mkspecs/wasm-emscripten"
    "-I$WORK"
)

# The .cpp includes its own moc output; -p keeps that include resolvable.
"$QT_HOST/libexec/moc" -p QtCore/private "${INCLUDES[@]}" \
    "$PRIVATE/qeventdispatcher_wasm_p.h" \
    -o "$WORK/moc_qeventdispatcher_wasm_p.cpp"

# QT_BUILDING_QT for the logging namespace; -fexceptions for the libc++ ABI tags.
em++ -c "$WORK/qeventdispatcher_wasm.cpp" -o "$WORK/$MEMBER" \
    -std=c++20 -O2 -fexceptions \
    -DQT_NO_DEBUG -DNDEBUG -DQT_BUILD_CORE_LIB -DQT_STATIC -DQT_BUILDING_QT \
    "${INCLUDES[@]}"

# Symbols in the archive today, whether or not this script put them there.
mkdir -p "$WORK/current"
(cd "$WORK/current" && emar x "$ARCHIVE" "$MEMBER")
reference="$WORK/current/$MEMBER"
if [ -f "$STOCK" ]; then
    mkdir -p "$WORK/stock"
    (cd "$WORK/stock" && emar x "$STOCK" "$MEMBER")
    reference="$WORK/stock/$MEMBER"
fi
llvm-nm --defined-only --extern-only "$reference" | awk '{print $NF}' | sort > "$WORK/sym.stock"
llvm-nm --defined-only --extern-only "$WORK/$MEMBER" | awk '{print $NF}' | sort > "$WORK/sym.built"
missing="$(comm -23 "$WORK/sym.stock" "$WORK/sym.built")"
extra="$(comm -13 "$WORK/sym.stock" "$WORK/sym.built")"
if [ -n "$missing" ] || [ -n "$extra" ]; then
    echo "error: the rebuilt object does not export the same symbols as the shipped one." >&2
    echo "       Swapping it in would change QtCore's link surface, so refusing." >&2
    [ -n "$missing" ] && echo "  missing:" && echo "$missing" | sed 's/^/    /' >&2
    [ -n "$extra" ] && echo "  extra:" && echo "$extra" | sed 's/^/    /' >&2
    exit 1
fi
echo "symbol check: the rebuilt object is a drop-in for the shipped one."

if [ "$action" = "verify" ]; then
    exit 0
fi

if [ ! -f "$STOCK" ]; then
    cp -a "$ARCHIVE" "$STOCK"
    echo "kept the stock archive at $STOCK"
fi
emar r "$ARCHIVE" "$WORK/$MEMBER"
emranlib "$ARCHIVE"
echo "patched $ARCHIVE"
echo "Relink anything already built against it (rm the .wasm and build again)."
