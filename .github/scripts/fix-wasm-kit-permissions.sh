#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Restore the executable bit on the Qt WebAssembly kit's shell scripts. The kit's archives are
# built on Windows and carry no POSIX permission bits, so qt-cmake and its siblings land 0644.

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
        if [ "$(head -c 2 "$file" 2>/dev/null)" = "#!" ] && [ ! -x "$file" ]; then
            chmod +x "$file"
            echo "  +x $(basename "$file")"
            fixed=$((fixed + 1))
        fi
    done
done

echo "restored the executable bit on $fixed script(s) in $kit"
