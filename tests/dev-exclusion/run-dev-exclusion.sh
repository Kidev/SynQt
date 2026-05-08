#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Development-only code is absent from a release build, not disabled inside it.
#
# The distinction is the whole suite. A capability held back by an `if (devMode)` is still
# in the binary. It can be reached through a bug in whatever checks the flag, through an
# argument someone passes, or read out of the strings by anyone holding the artifact.
# SynQt's claim is stronger than that, so this proves the stronger thing: a release
# SynQtEdge does not contain the development sign-in at all.
#
# Three assertions, and the second is not padding. A test that only checks for absence
# passes on a build that contains nothing, which is how `identity.required` refused
# everybody in this tree for months. So the middle one builds the same library with the
# option on and requires the symbol to be there.
#
#   QT_HOST=/path/to/qt/gcc_64 tests/dev-exclusion/run-dev-exclusion.sh

set -euo pipefail

case "$(uname -s)" in
Darwin) QT_HOST_DEFAULT=/opt/Qt/6.11.1/macos ;;
*)      QT_HOST_DEFAULT=/opt/Qt/6.11.1/gcc_64 ;;
esac
QT_HOST="${QT_HOST:-$QT_HOST_DEFAULT}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_ROOT="${BUILD_DIR:-build/dev-exclusion}"
rm -rf "$BUILD_ROOT"
# The logs sit beside the two build directories rather than inside them, so a configure that
# fails before creating its directory still has somewhere to say why.
mkdir -p "$BUILD_ROOT"

# The symbols this suite is about. One name per development-only type, so adding another is
# adding a word here rather than writing a second test.
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

# The symbol table of a static archive. `nm -C` demangles, so a name is matched as written
# rather than as the compiler spelled it. On a stripped archive this reads the object
# symbols, which is what a linker would use and therefore what "is it in there" means.
names_in() {
    nm -C "$1" 2>/dev/null || true
}

fail=0
note() { echo "  $*"; }

# The three checks are named functions rather than a straight line of script, so
# tests/security/attacks.json can name one the way it names a Qt test slot or a pytest
# function, and tools/synqt/tests/test_security_index.py can find the declaration.

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
# Without this the suite above would pass on an archive that contains nothing at all, which
# is the failure mode of every test written only as a refusal.
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
# The second layer. Without it, a translation unit that included a development header in a
# release build would compile and fail at link, naming a symbol rather than the mistake.
probe="$BUILD_ROOT/probe.cpp"
printf '#include "stubidentityserver.h"\nint main() { return 0; }\n' > "$probe"
# No Qt include path, because the guard sits above every #include in that header, so
# a compiler that cannot find one Qt header yet still refuses here is the guard working. If
# this ever starts failing on a missing QtCore include, the guard has moved below them.
if "${CXX:-c++}" -fsyntax-only -std=c++20 -I src/edge \
        "$probe" > "$BUILD_ROOT/probe.log" 2>&1; then
    note "FAIL stubidentityserver.h compiled with no SYNQT_DEV_TOOLS defined"
    fail=1
elif grep -q "development-only" "$BUILD_ROOT/probe.log"; then
    note "ok   the header refused, and said why"
else
    # It failed for some other reason (a missing Qt include, say), which proves nothing
    # about the tripwire.
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
