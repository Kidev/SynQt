#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Measure the transfer weight of a built WebAssembly client bundle: for every servable asset
# (.wasm, .js, .html, .data, .css, .svg) report the raw size and what gzip and brotli compress it
# to, since that compressed number is what actually crosses the wire on first load. Writes a JSON
# object to the path given by --out and a readable summary to stderr. brotli is optional; if it is not
# installed the brotli figures are reported as null rather than a wrong number.
#
#   measure-bundle.sh <bundle-dir> <label> --out <file.json>

set -euo pipefail

BUNDLE_DIR="${1:?usage: measure-bundle.sh <bundle-dir> <label> --out <file.json>}"
LABEL="${2:?missing label}"
shift 2
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        *) echo "measure-bundle: unknown arg $1" >&2; exit 2 ;;
    esac
done

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
json="$(printf '{"label":"%s","dir":"%s","files":[%s],"total_raw":%s,"total_gzip":%s,"total_brotli":%s}' \
    "$LABEL" "$BUNDLE_DIR" "$files_json" "$total_raw" "$total_gzip" "$total_brotli_field")"

if [ -n "$OUT" ]; then
    printf '%s\n' "$json" > "$OUT"
    echo "wrote $OUT" >&2
else
    printf '%s\n' "$json"
fi
