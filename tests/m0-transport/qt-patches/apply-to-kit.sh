#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Applies the QEventDispatcherWasm posted-event patch to an installed Qt WASM kit, and
# takes it back off again. See README.md beside this script for what the patch is and why.
#
# A prebuilt Qt kit is a set of static archives, so a one-file change does not need Qt
# rebuilt: recompile that one translation unit against the kit's own installed headers and
# swap the object into libQt6Core.a. The compile flags below were chosen by comparing the
# resulting object's exported symbols against the object already in the archive until the
# two sets matched exactly, which is the check that the replacement is a drop-in and not a
# differently-configured build of the same file. `--verify` re-runs that comparison.
#
# The stock archive is kept beside the patched one as libQt6Core.a.stock, so `--revert` is a
# copy rather than a rebuild, and so it is obvious from a directory listing that this kit is
# not the one Qt shipped.

set -euo pipefail

QT_WASM="${QT_WASM:-/opt/Qt/6.11.1/wasm_singlethread}"
QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
QT_SRC="${QT_SRC:-/opt/Qt/6.11.1/Src/qtbase}"

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

PRIVATE="$QT_WASM/include/QtCore/6.11.1/QtCore/private"
INCLUDES=(
    "-I$QT_WASM/include"
    "-I$QT_WASM/include/QtCore"
    "-I$QT_WASM/include/QtCore/6.11.1"
    "-I$QT_WASM/include/QtCore/6.11.1/QtCore"
    "-I$PRIVATE"
    "-I$QT_WASM/mkspecs/wasm-emscripten"
    "-I$WORK"
)

# The .cpp includes its own moc output, which an installed kit does not ship. -p keeps the
# generated include line resolvable from the kit's include path rather than relative to
# wherever this script happens to run.
"$QT_HOST/libexec/moc" -p QtCore/private "${INCLUDES[@]}" \
    "$PRIVATE/qeventdispatcher_wasm_p.h" \
    -o "$WORK/moc_qeventdispatcher_wasm_p.cpp"

# QT_BUILDING_QT is what puts the logging categories in the QtPrivateLogging inline
# namespace, so leaving it out silently produces differently-mangled symbols that the rest of
# QtCore does not reference. -fexceptions matches the shipped object's libc++ ABI tags.
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
