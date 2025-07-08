# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The build manifest: the client build id and module size.

The boot script reads ``wasm_size`` for a determinate percentage; ``Content-Length`` is the
Brotli size while the stream is decoded. The service worker reads ``build_id`` to detect a
newer client.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any, Dict

MANIFEST_NAME = "synqt-manifest.json"

# Precompressed variants are not listed: the edge picks an encoding per request.
_DERIVED_SUFFIXES = (".br", ".gz")


def build_id(wasm_path: Path) -> str:
    """The client identity: the sha256 of its WebAssembly module, so an unchanged rebuild keeps
    its id.
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
