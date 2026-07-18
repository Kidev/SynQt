# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The build manifest: what this client build is, and how big its module really is.

Two consumers, one file. The boot script reads ``wasm_size`` to show a determinate
percentage, and the service worker reads ``build_id`` to decide whether the edge holds a
newer client than the one in its cache.

``wasm_size`` exists because the obvious source is wrong. The edge serves the module
with ``Content-Encoding: br``, so the browser's ``Content-Length`` reports the
compressed size while the stream the boot script counts is the decoded one. Dividing
one by the other yields a percentage that races past 100. This records the uncompressed
size at build time, where it is known for certain.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any, Dict

MANIFEST_NAME = "synqt-manifest.json"

# Precompressed variants are derived artifacts: the worker precaches logical URLs and
# the edge picks an encoding per request, so listing them would cache the same bytes
# twice under two names.
_DERIVED_SUFFIXES = (".br", ".gz")


def build_id(wasm_path: Path) -> str:
    """The client's identity: the sha256 of its WebAssembly module.

    Content addressed rather than a timestamp or a counter, so an unchanged rebuild
    produces an unchanged id and does not push a pointless update to every browser.
    """
    digest = hashlib.sha256()
    with Path(wasm_path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def manifest(client_dir: Path, wasm_name: str) -> Dict[str, Any]:
    """The manifest for an assembled bundle directory."""
    client_dir = Path(client_dir)
    wasm = client_dir / wasm_name
    files = sorted(
        entry.name for entry in client_dir.iterdir()
        if entry.is_file()
        and entry.name != MANIFEST_NAME
        and not entry.name.endswith(_DERIVED_SUFFIXES)
    )
    return {
        "build_id": build_id(wasm),
        "wasm": wasm_name,
        "wasm_size": wasm.stat().st_size,
        "files": files,
    }


def write(client_dir: Path, wasm_name: str) -> Path:
    """Write ``synqt-manifest.json`` into the bundle and return its path."""
    path = Path(client_dir) / MANIFEST_NAME
    payload = json.dumps(manifest(client_dir, wasm_name), indent=2, sort_keys=True)
    path.write_text(payload + "\n")
    return path
