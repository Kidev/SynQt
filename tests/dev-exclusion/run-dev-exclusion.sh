#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Development-only code is absent from a release build, not disabled inside it: a release
# SynQtEdge does not contain the development sign-in. The middle check builds with the option
# on and requires the symbol, so the absence check cannot pass on an empty archive.
#
#   QT_HOST=/path/to/qt/gcc_64 tests/dev-exclusion/run-dev-exclusion.sh

set -euo pipefail

case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.12.0/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.12.0/gcc_64 ;;
esac
QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_ROOT="${BUILD_DIR:-build/dev-exclusion}"
rm -rf "$BUILD_ROOT"
# Logs beside the build directories, so a failed configure still leaves one.
mkdir -p "$BUILD_ROOT"

# One symbol per development-only type.
DEV_SYMBOLS="StubIdentityServer IdentityPicker"

configure_and_build() {  # directory, SYNQT_DEV_TOOLS value
    cmake -S . -B "$1" -G Ninja \
        -DCMAKE_PREFIX_PATH="$QT_HOST" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSYNQT_DEV_TOOLS="$2" > "$1.log" 2>&1
    cmake --build "$1" --target SynQtEdge >> "$1.log" 2>&1
}

archive_of() {
    find "$1" -name 'libSynQtEdge.a' -o -name 'SynQtEdge.lib' | head -1
}

# The archive's symbol table, demangled.
names_in() {
    nm -C "$1" 2>/dev/null || true
}

fail=0
note() { echo "  $*"; }

# Named functions, so tests/security/attacks.json can cite each one.

aReleaseEdgeDoesNotContainTheDevelopmentSignIn() {
echo "== [1/3] a release SynQtEdge does not contain the development sign-in =="
configure_and_build "$BUILD_ROOT/release" OFF
release_archive="$(archive_of "$BUILD_ROOT/release")"
if [ -z "$release_archive" ]; then
    echo "FAIL: SynQtEdge did not build with SYNQT_DEV_TOOLS=OFF (see $BUILD_ROOT/release.log)"
    exit 1
fi
for symbol in $DEV_SYMBOLS; do
    if names_in "$release_archive" | grep -q "$symbol"; then
        note "FAIL $symbol is in $release_archive"
        fail=1
    else
        note "ok   $symbol is absent"
    fi
done

}

aDevelopmentEdgeDoesContainIt() {
echo "== [2/3] and the development build does contain it =="
configure_and_build "$BUILD_ROOT/dev" ON
dev_archive="$(archive_of "$BUILD_ROOT/dev")"
if [ -z "$dev_archive" ]; then
    echo "FAIL: SynQtEdge did not build with SYNQT_DEV_TOOLS=ON (see $BUILD_ROOT/dev.log)"
    exit 1
fi
for symbol in $DEV_SYMBOLS; do
    if names_in "$dev_archive" | grep -q "$symbol"; then
        note "ok   $symbol is present when it was asked for"
    else
        note "FAIL $symbol is absent even with SYNQT_DEV_TOOLS=ON, so [1/3] proves nothing"
        fail=1
    fi
done

}

aDevelopmentHeaderRefusesToBeIncludedWithoutTheOption() {
echo "== [3/3] a development header refuses to be included without the option =="
# The second layer: a development header refuses to compile in a release build.
probe="$BUILD_ROOT/probe.cpp"
printf '#include "stubidentityserver.h"\nint main() { return 0; }\n' > "$probe"
# No Qt include path: the guard sits above every #include in that header.
if "${CXX:-c++}" -fsyntax-only -std=c++20 -I src/edge \
        "$probe" > "$BUILD_ROOT/probe.log" 2>&1; then
    note "FAIL stubidentityserver.h compiled with no SYNQT_DEV_TOOLS defined"
    fail=1
elif grep -q "development-only" "$BUILD_ROOT/probe.log"; then
    note "ok   the header refused, and said why"
else
    # A failure for any other reason proves nothing.
    note "FAIL the header did not compile, but not because of its own guard:"
    sed -n '1,5p' "$BUILD_ROOT/probe.log" | sed 's/^/       /'
    fail=1
fi

}

aReleaseEdgeDoesNotContainTheDevelopmentSignIn
aDevelopmentEdgeDoesContainIt
aDevelopmentHeaderRefusesToBeIncludedWithoutTheOption

echo
if [ "$fail" = 0 ]; then
    echo "DEV EXCLUSION: PASS (development code is absent from a release build, not disabled in it)"
else
    echo "DEV EXCLUSION: FAIL"
fi
exit "$fail"
