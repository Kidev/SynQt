# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Command-line entry point: ``synqtc <input.syn> --out <dir>``.

Generates, into ``--out``, ``<stem>.rep`` plus the Source helper
(``<stem>_sourcehelper.{h,cpp}``) and the Replica registration
(``<stem>_replica.{h,cpp}``). On malformed input it prints a
``path:line:col: error: message`` diagnostic and exits non-zero.
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional

from . import emit
from .errors import SynError
from .parser import parse_file


def _write(path: str, content: str) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)


def generate(input_path: str, out_dir: str) -> List[str]:
    """Parse ``input_path`` and write the generated artifacts into ``out_dir``.

    Returns the list of written file paths. Raises :class:`SynError` on malformed
    input.
    """
    syn = parse_file(input_path)
    lstem = syn.stem.lower()
    os.makedirs(out_dir, exist_ok=True)

    outputs = {
        f"{lstem}.rep": emit.emit_rep(syn),
        f"{lstem}_sourcehelper.h": emit.emit_source_helper_header(syn, lstem),
        f"{lstem}_sourcehelper.cpp": emit.emit_source_helper_source(syn, lstem),
        f"{lstem}_replica.h": emit.emit_replica_header(syn, lstem),
        f"{lstem}_replica.cpp": emit.emit_replica_source(syn, lstem),
        f"{lstem}_consumer.h": emit.emit_consumer_header(syn, lstem),
        f"{lstem}_consumer.cpp": emit.emit_consumer_source(syn, lstem),
    }
    written: List[str] = []
    for name, content in outputs.items():
        target = os.path.join(out_dir, name)
        _write(target, content)
        written.append(target)
    return written


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="synqtc",
        description="Lower a SynQt .syn contract to a QtRO rep plus QML registrations.",
    )
    parser.add_argument("inputs", nargs="+", metavar="INPUT.syn", help="contract files to compile")
    parser.add_argument("--out", required=True, metavar="DIR", help="output directory")
    parser.add_argument("--quiet", action="store_true", help="do not list written files")
    args = parser.parse_args(argv)

    for input_path in args.inputs:
        if not os.path.isfile(input_path):
            print(f"{input_path}: error: no such file", file=sys.stderr)
            return 1
        try:
            written = generate(input_path, args.out)
        except SynError as error:
            print(error.format(), file=sys.stderr)
            return 1
        if not args.quiet:
            for path in written:
                print(path)
    return 0
