#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Prove the custom provider path, which is the framework's expandability escape hatch, using
# the real output of `synqt add provider`. The Python tests assert the scaffolded strings;
# this fixture compiles them, links them, and drives the factories, which is the only thing
# that proves a user who follows docs/providers.md ends up with a selectable provider.
#
# It matters because this path was documented and scaffolded long before it existed: the docs
# said "register it under a name" with no registry to register with, and a
# `provider.name: custom:X` resolved to nullptr in silence.
#
# Step 1 scaffolds one provider per family into build/custom-provider/scaffold/ (nothing is
# committed: the point is to build what the tool emits today, not a copy of it). Step 2 builds
# them into a test that asserts each registered itself and that its family factory selects it.
#
# Needs the pinned host kit (/opt/Qt/6.11.1/gcc_64). Usage:
#   tests/custom-provider/run-customprovider.sh

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
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

# One provider per family, each named distinctly so the test can tell them apart in the
# registry. scaffold() writes to <project>/providers/custom/, so give each its own project
# and collect the results; that keeps this honest about what the tool actually emits.
for name, family in (("MyStore", "persistence"), ("MyCache", "cache"), ("MyDocs", "document")):
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
