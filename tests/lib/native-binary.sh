# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Shared helpers for asserting that a build produced a real native executable, on any host
# SynQt targets as a desktop platform (Linux, macOS, Windows; see docs/desktop.md).
#
# Sourced, not executed: `. "$REPO_ROOT/tests/lib/native-binary.sh"`.
#
# Two things here are deliberate.
#
# The executable's name is asked for without a suffix, because only Windows adds one (.exe). A
# test that hard-codes the bare name reports MISSING on Windows for a binary that linked fine.
#
# The kind is read from the file's magic number rather than from `file`'s prose. `file` is not in
# every Git-for-Windows install, and its wording is neither stable across versions nor the same on
# two platforms, which is exactly how `file -b ... | grep -q ELF` came to report MISSING on macOS
# for three executables that had just linked successfully: the assertion only ever recognised
# Linux, so the one thing it proved was that the test ran on Linux.

# Echo the path of a built executable, accounting for the .exe suffix on Windows. Echoes nothing
# when neither exists, so callers can test for an empty result.
#
# The .exe variant is tried FIRST, and that order is load-bearing on Windows. Git-for-Windows
# bash is Cygwin/MSYS2, whose stat() transparently resolves a bare name to its `.exe` sibling
# (the "exe magic"), so `[ -f "$1" ]` is TRUE for a target that only exists as `$1.exe` -- and
# this function would then echo the bare, extension-less path. That path works in bash (od, test)
# but is a plain FileNotFoundError to any NATIVE Windows program, which has no exe magic: it cost
# a CI round when a native Python step downstream tried to open the returned path and failed on a
# binary that had linked perfectly. Preferring `$1.exe` returns the real filename; on Linux/macOS
# there is no `.exe`, so it falls through to the bare name unchanged.
native_exe_path() {
    if [ -f "$1.exe" ]; then
        printf '%s\n' "$1.exe"
    elif [ -f "$1" ]; then
        printf '%s\n' "$1"
    fi
}

# Echo a short binary kind label (ELF, Mach-O, PE), or nothing if the file is not a
# native executable for any platform we build for.
native_exe_kind() {
    magic="$(od -A n -t x1 -N 4 "$1" 2>/dev/null | tr -d ' \n')"
    case "$magic" in
        7f454c46)                    printf 'ELF\n' ;;
        # Mach-O, thin: 0xfeedfacf (64-bit) / 0xfeedface (32-bit), byte-swapped on disk on a
        # little-endian host, so accept both orders.
        cffaedfe|cefaedfe|feedfacf|feedface) printf 'Mach-O\n' ;;
        # Mach-O, universal ("fat"): what a default macOS build of a Qt app usually is.
        cafebabe|bebafeca)           printf 'Mach-O universal\n' ;;
        # PE/COFF starts with the DOS stub's "MZ"; the two bytes after it vary.
        4d5a*)                       printf 'PE\n' ;;
        *)                           printf '' ;;
    esac
}

# Assert one built executable exists and is native. Prints an aligned OK/MISSING line, and
# returns non-zero on failure so the caller can accumulate a result.
#   assert_native_exe <path-without-suffix> <label>
assert_native_exe() {
    _path="$(native_exe_path "$1")"
    _label="${2:-$(basename "$1")}"
    if [ -z "$_path" ]; then
        printf '  %s : MISSING (no executable at %s)\n' "$_label" "$1"
        # Say what is there instead. "It is not at the path I expected" and "it was never
        # built" are different failures with the same message here, and telling them apart
        # cost a CI round trip: on Windows the binaries were built perfectly well into a
        # per-config subdirectory (build/host/Debug/) that nothing downstream looked in, and
        # this line reported the same MISSING it would have for a compile that never ran.
        if [ -d "$(dirname "$1")" ]; then
            printf '            (%s contains: %s)\n' "$(dirname "$1")" \
                "$(ls "$(dirname "$1")" 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"
        else
            printf '            (%s does not exist)\n' "$(dirname "$1")"
        fi
        return 1
    fi
    _kind="$(native_exe_kind "$_path")"
    if [ -z "$_kind" ]; then
        printf '  %s : NOT A NATIVE EXECUTABLE (%s)\n' "$_label" "$_path"
        return 1
    fi
    printf '  %s : OK (%s)\n' "$_label" "$_kind"
    return 0
}
