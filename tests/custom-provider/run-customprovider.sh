#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build and drive what `synqt add provider` scaffolds: one provider per family, each of
# which must register itself and be selected by its family factory.
#
# Usage: tests/custom-provider/run-customprovider.sh

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.12.0/gcc_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x "$QT_HOST/bin/qmake" ] && [ ! -d "$QT_HOST/lib/cmake" ]; then
    echo "error: native host kit not found at $QT_HOST" >&2
    exit 1
fi

WORK="$REPO_ROOT/build/custom-provider"
SCAFFOLD="$WORK/scaffold"

echo "== [1/3] Scaffold one custom provider per family with the real CLI code path =="
rm -rf "$WORK"
mkdir -p "$SCAFFOLD"
PYTHONPATH="$REPO_ROOT/tools/synqt" python3 - "$WORK" "$SCAFFOLD" <<'PY'
import shutil
import sys
from pathlib import Path

from synqt import addprovider

work, scaffold = Path(sys.argv[1]), Path(sys.argv[2])

# One provider per family, each in its own project, named distinctly.
for name, family in (("MyStore", "relational"), ("MyCache", "cache"), ("MyDocs", "document")):
    project = work / f"project-{family}"
    project.mkdir(parents=True, exist_ok=True)
    addprovider.scaffold(project, name, family)
    source = project / "providers" / "custom" / f"{name.lower()}provider.cpp"
    shutil.copy(source, scaffold / source.name)
    print(f"  scaffolded {family:11s} -> {source.name}")
PY

echo "== [2/3] Compile the scaffolded providers into a test and link them =="
cmake -S tests/custom-provider -B "$WORK/build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$QT_HOST" \
    -DSYNQT_SCAFFOLD_DIR="$SCAFFOLD" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$WORK/build"

echo "== [3/3] Each scaffolded provider registers itself and is selectable by config =="
ctest --test-dir "$WORK/build" --output-on-failure
