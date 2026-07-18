#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Restore the executable bit on the Qt WebAssembly kit's shell scripts.
#
# The WASM kit is one host-independent build, published under aqt's all_os/wasm host and target,
# and the archives it is packaged from are built on Windows (the payload names say so:
# qtbase-Windows-Windows_11_24H2-Clang-Windows-WebAssembly-X86_64.7z, and the kit ships .bat
# siblings next to every script). Windows has no POSIX permission bits to record, so the 7z
# carries none, and the scripts land 0644 on Linux and macOS. The kit is not broken: it just
# cannot describe a Unix permission it never had. aqt's own patched files (qmake, qmake6) are
# executable precisely because aqt rewrites those itself.
#
# The symptom is "qt-cmake: Permission denied" and exit 126, which reads like a missing file or a
# bad path rather than a missing bit. Fixing every shebang script, rather than chmod-ing qt-cmake
# by name, keeps this from recurring one script at a time (qt-cmake-create and
# qt-configure-module land unexecutable too, and a later Qt may add more).

set -euo pipefail

kit="${1:?usage: fix-wasm-kit-permissions.sh <path-to-wasm-kit>}"

if [ ! -d "$kit" ]; then
    echo "no such kit: $kit" >&2
    exit 1
fi

fixed=0
for dir in "$kit/bin" "$kit/libexec"; do
    [ -d "$dir" ] || continue
    for file in "$dir"/*; do
        [ -f "$file" ] || continue
        case "$file" in
            *.bat|*.conf|*.json|*.html|*.js|*.cmake) continue ;;
        esac
        # A shebang is the test: it is what makes the file something the shell must exec, and it
        # is true of the scripts regardless of what Qt names them in a future version.
        if [ "$(head -c 2 "$file" 2>/dev/null)" = "#!" ] && [ ! -x "$file" ]; then
            chmod +x "$file"
            echo "  +x $(basename "$file")"
            fixed=$((fixed + 1))
        fi
    done
done

echo "restored the executable bit on $fixed script(s) in $kit"

# Not a no-op guard: if a future kit ships these correctly, this script should stop existing
# rather than silently pass. Nothing here fails the build on 0, because the desktop kits are
# unaffected and this same script runs against them in some jobs.
