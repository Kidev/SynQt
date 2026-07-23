#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Measure the first-load saving of edge-delivered pages. The `stall` storefront (examples/stall)
# keeps its campaign pages as `remote:` routes: the edge delivers Campaign.qml and Members.qml on
# demand, so they are never compiled into the client bundle and never cross the wire on a first
# visit. This harness builds the same example twice through the real `synqt build` path (once as
# written, once with those two pages rewritten to compiled-in `view:` routes) and weighs each
# client bundle with the shared measure-bundle.sh (raw / gzip / brotli). The saving is the
# compiled-in weight minus the remote weight: the bytes the browser does not download on first
# load because the pages live on the edge instead.
#
# It builds the WebAssembly client, so it needs the Qt for WebAssembly kit and the pinned
# Emscripten (4.0.7), and belongs on a workstation with that toolchain, not the build sandbox.
# The single-threaded kit is enough for a bundle-weight baseline; the client bundle bytes are
# what matter here, not the thread count. Only the client entity is compiled, since the client
# bundle is the only artifact weighed.
#
#   benchmarks/remote-pages/run.sh [--out <file.json>]

set -euo pipefail

QT_WASM_ST="${QT_WASM_ST:-/opt/Qt/6.11.1/wasm_singlethread}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

HOST_TAG="$(hostname | tr -c 'A-Za-z0-9_.-' '_')"
RESULTS_DIR="benchmarks/results"
OUT="${RESULTS_DIR}/remote-pages-${HOST_TAG}.json"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        *) echo "run: unknown arg $1" >&2; exit 2 ;;
    esac
done

if [ ! -x "$QT_WASM_ST/bin/qt-cmake" ]; then
    echo "run: single-threaded WASM kit not found at $QT_WASM_ST" >&2
    echo "     set QT_WASM_ST to a Qt 6.11.1 wasm_singlethread kit and re-run." >&2
    exit 1
fi

# The CLI, run from this repo checkout so the generated CMake points SYNQT_ROOT at this
# repo's src/ and cmake/ (framework_root() is derived from the module's own path). An
# installed synqt elsewhere on PATH would bake in its own checkout, not this one.
synqt_cli() {
    PYTHONPATH="$REPO_ROOT/tools/synqt" python3 -m synqt "$@"
}

EXAMPLE="$REPO_ROOT/examples/stall"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# The two variants, copied out of the tree so the committed example is never touched.
#   remote      : examples/stall as written (campaign pages are edge-delivered).
#   compiled-in : the two remote routes rewritten to `view:` and their page QML placed in the
#                 client directory, so qmlcachegen compiles them into the first-load bundle.
cp -a "$EXAMPLE" "$WORK/remote"
cp -a "$EXAMPLE" "$WORK/compiled-in"

python3 - "$WORK/compiled-in" <<'PY'
import pathlib
import shutil
import sys

import yaml

project = pathlib.Path(sys.argv[1])
config = yaml.safe_load((project / "synqt.yaml").read_text())
client = next(entity["name"] for entity in config["entities"]
              if entity.get("kind") == "client")
edge = next(entity["name"] for entity in config["entities"]
            if entity.get("capability") == "web_edge")
for route in config.get("routes", []):
    page = route.get("remote")
    if not page:
        continue
    # A compiled-in view is named relative to the client directory, so the page QML moves
    # there; the edge no longer delivers it, so `remote:` and its `seed:` are dropped.
    shutil.copy2(project / edge / "pages" / page, project / client / page)
    route["view"] = page
    route.pop("remote", None)
    route.pop("seed", None)
(project / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
PY

# Build one variant's WebAssembly client and weigh the assembled bundle. $1 = project dir,
# $2 = label, $3 = output JSON from measure-bundle.sh.
build_and_measure() {
    local project="$1"
    local label="$2"
    local out="$3"
    echo "== build client ($label) ==" >&2
    synqt_cli build --client wasm --entity client --project-dir "$project" --release >&2
    bash "$REPO_ROOT/benchmarks/client/measure-bundle.sh" \
        "$project/build/client" "$label" --out "$out"
}

build_and_measure "$WORK/remote" "stall-remote" "$WORK/remote.json"
build_and_measure "$WORK/compiled-in" "stall-compiled-in" "$WORK/compiled-in.json"

# The Emscripten version reported by the kit's emcc, falling back to the pinned 4.0.7.
EMSCRIPTEN_VERSION="$(emcc --version 2>/dev/null | sed -n '1s/.*replacement + linker emulating GNU ld) \([0-9.]*\).*/\1/p')"
EMSCRIPTEN_VERSION="${EMSCRIPTEN_VERSION:-4.0.7}"
QT_VERSION="$(python3 -c 'import yaml,sys; print(yaml.safe_load(open(sys.argv[1]))["project"]["qt_version"])' "$EXAMPLE/synqt.yaml")"

mkdir -p "$(dirname "$OUT")"
python3 - "$WORK/remote.json" "$WORK/compiled-in.json" "$OUT" \
    "$HOST_TAG" "$(uname -m)" "$QT_VERSION" "$EMSCRIPTEN_VERSION" <<'PY'
import datetime
import json
import sys

remote_path, compiled_path, out_path, host, arch, qt_version, emscripten = sys.argv[1:8]
remote = json.load(open(remote_path))
compiled = json.load(open(compiled_path))


def summary(measured):
    return {
        "label": measured["label"],
        "total_raw": measured["total_raw"],
        "total_gzip": measured["total_gzip"],
        "total_brotli": measured["total_brotli"],
        "file_count": len(measured["files"]),
        "files": measured["files"],
    }


# Positive means the compiled-in variant is heavier, so keeping the pages on the edge saves
# that many bytes on a first visit.
saving = {
    "raw_bytes": compiled["total_raw"] - remote["total_raw"],
    "gzip_bytes": compiled["total_gzip"] - remote["total_gzip"],
    "brotli_bytes": (None if compiled["total_brotli"] is None or remote["total_brotli"] is None
                     else compiled["total_brotli"] - remote["total_brotli"]),
    "file_count_delta": len(compiled["files"]) - len(remote["files"]),
}

baseline = {
    "benchmark": "remote-pages",
    "path": "first-load-saving-of-edge-delivered-pages",
    "example": "examples/stall",
    "host": host,
    "arch": arch,
    "qt_version": qt_version,
    "emscripten_version": emscripten,
    "kit": "wasm_singlethread",
    "recorded": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
    "remote": summary(remote),
    "compiled_in": summary(compiled),
    "saving": saving,
}
with open(out_path, "w") as handle:
    json.dump(baseline, handle, indent=2)
    handle.write("\n")
print(f"wrote {out_path}", file=sys.stderr)
print(json.dumps(baseline["saving"], indent=2))
PY

echo "== done. baseline at $OUT ==" >&2
