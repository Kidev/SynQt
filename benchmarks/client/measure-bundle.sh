#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Measure the transfer weight of a built WebAssembly client bundle. For every servable asset
# (.wasm, .js, .html, .data, .css, .svg) report the raw size and what gzip and brotli compress it
# to, since that compressed number is what crosses the wire on first load. Writes a JSON
# object to the path given by --out and a readable summary to stderr. brotli is optional. If it is not
# installed the brotli figures are reported as null rather than a wrong number.
#
#   measure-bundle.sh <bundle-dir> <label> --out <file.json> [--qt-version 6.12.0]

set -euo pipefail

BUNDLE_DIR="${1:?usage: measure-bundle.sh <bundle-dir> <label> --out <file.json>}"
LABEL="${2:?missing label}"
shift 2
OUT=""
QT_VERSION="unknown"   # run-bench.sh asks the kit; a default here would be a second answer
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --qt-version) QT_VERSION="$2"; shift 2 ;;
        *) echo "measure-bundle: unknown arg $1" >&2; exit 2 ;;
    esac
done
# A weight written to a baseline says which Qt it was built with, and only the caller knows.
if [ -n "$OUT" ] && [ "$QT_VERSION" = "unknown" ]; then
    echo "measure-bundle: --qt-version is required with --out (the kit's version, read from" >&2
    echo "  <kit>/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake)" >&2
    exit 2
fi

# The same attribution every other harness records. A weight with no machine, no Qt, and no
# date behind it cannot be compared against a later one, which is the only thing a baseline
# is for.
HOST_LABEL="$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-$(uname -s)}" || uname -s)"
ARCH="$(uname -m)"
RECORDED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

if [ ! -d "$BUNDLE_DIR" ]; then
    echo "measure-bundle: no such bundle dir: $BUNDLE_DIR" >&2
    exit 1
fi

have_brotli=0
if command -v brotli >/dev/null 2>&1; then
    have_brotli=1
fi

total_raw=0
total_gzip=0
total_brotli=0
brotli_known=1
entries=()

echo "== bundle: $LABEL ($BUNDLE_DIR) ==" >&2
printf '%-28s %12s %12s %12s\n' "asset" "raw" "gzip" "brotli" >&2

while IFS= read -r file; do
    name="$(basename "$file")"
    raw="$(stat -c%s "$file")"
    gz="$(gzip -9 -c "$file" | wc -c)"
    if [ "$have_brotli" -eq 1 ]; then
        br="$(brotli -q 11 -c "$file" | wc -c)"
        total_brotli=$((total_brotli + br))
        br_field="$br"
    else
        brotli_known=0
        br="n/a"
        br_field="null"
    fi
    total_raw=$((total_raw + raw))
    total_gzip=$((total_gzip + gz))
    printf '%-28s %12s %12s %12s\n' "$name" "$raw" "$gz" "$br" >&2
    entries+=("$(printf '{"name":"%s","raw":%s,"gzip":%s,"brotli":%s}' \
        "$name" "$raw" "$gz" "$br_field")")
done < <(find "$BUNDLE_DIR" -type f \
    \( -name '*.wasm' -o -name '*.js' -o -name '*.html' -o -name '*.data' \
       -o -name '*.css' -o -name '*.svg' \) | sort)

total_brotli_field="$total_brotli"
brotli_summary="$total_brotli"
if [ "$brotli_known" -eq 0 ]; then
    total_brotli_field="null"
    brotli_summary="n/a (brotli not installed)"
fi
printf '%-28s %12s %12s %12s\n' "TOTAL" "$total_raw" "$total_gzip" "$brotli_summary" >&2

files_json="$(IFS=,; echo "${entries[*]}")"
json="$(printf '{"benchmark":"client-bundle","label":"%s","dir":"%s","host":"%s","arch":"%s",' \
    "$LABEL" "$BUNDLE_DIR" "$HOST_LABEL" "$ARCH")"
json="$json$(printf '"qt_version":"%s","recorded":"%s","files":[%s],' \
    "$QT_VERSION" "$RECORDED" "$files_json")"
json="$json$(printf '"total_raw":%s,"total_gzip":%s,"total_brotli":%s}' \
    "$total_raw" "$total_gzip" "$total_brotli_field")"

# Indent it if python is around. These files are committed baselines whose whole job is to
# be read in a diff when a later change moves a number, and one line holding every file's
# three sizes is a diff nobody can review. A single byte of drift rewrites the whole line.
# The other harnesses here already write indented JSON; this one was the odd one out.
if command -v python3 >/dev/null 2>&1; then
    indented="$(printf '%s' "$json" | python3 -c \
        'import json,sys; print(json.dumps(json.load(sys.stdin), indent=2))' 2>/dev/null)"
    [ -n "$indented" ] && json="$indented"
fi

if [ -n "$OUT" ]; then
    printf '%s\n' "$json" > "$OUT"
    echo "wrote $OUT" >&2
else
    printf '%s\n' "$json"
fi
