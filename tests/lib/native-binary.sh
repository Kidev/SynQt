# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Helpers asserting that a build produced a native executable on Linux, macOS or Windows.
# Sourced: `. "$REPO_ROOT/tests/lib/native-binary.sh"`.
#
# Names are given without the .exe suffix. The kind is read from the magic number, not from
# `file`, which is missing on Git for Windows and words its output differently per platform.

# Echo the path of a built executable, or nothing when it does not exist.
#
# `.exe` first: MSYS stat() resolves a bare name to its .exe sibling, and a native Windows
# program cannot open the bare name. On macOS the desktop client is a .app bundle, and the
# executable inside it is what runs.
native_exe_path() {
    if [ -f "$1.exe" ]; then
        printf '%s\n' "$1.exe"
    elif [ -f "$1" ]; then
        printf '%s\n' "$1"
    elif [ -f "$1.app/Contents/MacOS/$(basename "$1")" ]; then
        printf '%s\n' "$1.app/Contents/MacOS/$(basename "$1")"
    fi
}

# Echo a short binary kind label (ELF, Mach-O, PE), or nothing if the file is not a
# native executable for any platform SynQt builds for.
native_exe_kind() {
    magic="$(od -A n -t x1 -N 4 "$1" 2>/dev/null | tr -d ' \n')"
    case "$magic" in
        7f454c46)                    printf 'ELF\n' ;;
        # Mach-O, thin, 64- or 32-bit, in either byte order.
        cffaedfe|cefaedfe|feedfacf|feedface) printf 'Mach-O\n' ;;
        # Mach-O, universal ("fat"), what a default macOS build of a Qt app usually is.
        cafebabe|bebafeca)           printf 'Mach-O universal\n' ;;
        # PE/COFF starts with the DOS stub's "MZ". The two bytes after it vary.
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
        # List what is there instead, to tell a wrong path from a missing build.
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
