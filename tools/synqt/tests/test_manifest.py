# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The build manifest: the client's identity and the honest size of its module."""

import json
import tempfile
import unittest
from pathlib import Path

from synqt import manifest


def _bundle(root, wasm_bytes=b"\x00asm\x01\x00\x00\x00"):
    root.mkdir(parents=True, exist_ok=True)
    (root / "client.wasm").write_bytes(wasm_bytes)
    (root / "client.js").write_text("// glue")
    (root / "index.html").write_text("<!doctype html>")
    return root


class ManifestTest(unittest.TestCase):
    def test_records_the_uncompressed_wasm_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _bundle(Path(tmp) / "client", b"\x00asm" + b"x" * 100)
            data = manifest.manifest(root, "client.wasm")
        # Load-bearing: the edge serves the wasm with Content-Encoding: br, so a
        # browser's Content-Length is the compressed size while the stream the boot
        # script counts is decoded. Progress divides by this, not by Content-Length.
        self.assertEqual(data["wasm_size"], 104)
        self.assertEqual(data["wasm"], "client.wasm")

    def test_build_id_changes_only_when_the_wasm_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = manifest.manifest(_bundle(Path(tmp) / "a"), "client.wasm")
            same = manifest.manifest(_bundle(Path(tmp) / "b"), "client.wasm")
            other = manifest.manifest(
                _bundle(Path(tmp) / "c", b"\x00asm\x01\x00\x00\x01"), "client.wasm")
        self.assertEqual(first["build_id"], same["build_id"])
        self.assertNotEqual(first["build_id"], other["build_id"])

    def test_files_lists_the_bundle_but_not_the_encodings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _bundle(Path(tmp) / "client")
            (root / "client.wasm.br").write_bytes(b"brotli")
            (root / "client.wasm.gz").write_bytes(b"gzip")
            data = manifest.manifest(root, "client.wasm")
        self.assertIn("client.wasm", data["files"])
        self.assertIn("index.html", data["files"])
        # The worker precaches logical URLs; the edge picks the encoding per request.
        self.assertNotIn("client.wasm.br", data["files"])
        self.assertNotIn("client.wasm.gz", data["files"])

    def test_write_emits_json_excluding_itself(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = _bundle(Path(tmp) / "client")
            path = manifest.write(root, "client.wasm")
            data = json.loads(path.read_text())
        self.assertEqual(path.name, "synqt-manifest.json")
        self.assertNotIn("synqt-manifest.json", data["files"])


if __name__ == "__main__":
    unittest.main()
