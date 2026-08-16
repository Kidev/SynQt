#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Line coverage for the SynQt runtime libraries, read back from gcov.

`cmake -DSYNQT_COVERAGE=ON` instruments the five libraries under src/
(cmake/SynQtCoverage.cmake). After the suites run, this reads the .gcda files through `gcov
-t -j`, keeps SynQt's sources, and reports the fraction of executable lines reached.

    tools/coverage/report.py --build-dir build/coverage [--fail-under 70]

`--json` writes the figures for CI. Code under `#ifdef Q_OS_WASM` is not compiled natively,
so it is counted and named separately instead of silently leaving the denominator. A line that
is only a Qt declaration macro (Q_ENUM, Q_DECLARE_METATYPE and the like) is not counted: its
code is what the macro writes. gcov names every header a unit includes, and a file left with
no executable line is not reported.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Set, Tuple


def _gcov_documents(gcov: str, gcda_files, build_dir: Path):
    """Every gcov JSON document for these .gcda files, in bounded batches."""
    batch = []
    for gcda in gcda_files:
        batch.append(str(gcda))
        if len(batch) >= 64:
            yield from _run_gcov(gcov, batch, build_dir)
            batch = []
    if batch:
        yield from _run_gcov(gcov, batch, build_dir)


def _run_gcov(gcov: str, batch, build_dir: Path):
    """Read one batch; if gcov fails on it, read its files one at a time, so one unreadable
    counter file costs one file, not the whole report.
    """
    result = subprocess.run([gcov, "--stdout", "--json-format", *batch],
                            cwd=build_dir, capture_output=True, text=True)
    if result.returncode == 0:
        yield from _documents_in(result.stdout)
        return
    if len(batch) == 1:
        print("warning: gcov could not read %s: %s"
              % (batch[0], (result.stderr.strip().splitlines() or ["no message"])[-1]),
              file=sys.stderr)
        return
    for gcda in batch:
        yield from _run_gcov(gcov, [gcda], build_dir)


def _documents_in(output: str):
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            print("warning: gcov produced a line that is not JSON", file=sys.stderr)


# A line that is only one of these declares something, and Qt writes the code gcov counts
# there.
_DECLARATION_MACRO = re.compile(
    r"^\s*(Q_OBJECT|Q_GADGET|Q_GADGET_EXPORT|Q_NAMESPACE|Q_NAMESPACE_EXPORT|Q_ENUM|Q_ENUM_NS"
    r"|Q_FLAG|Q_FLAG_NS|Q_DECLARE_METATYPE|Q_DECLARE_FLAGS|Q_DECLARE_OPERATORS_FOR_FLAGS"
    r"|Q_DECLARE_TYPEINFO|Q_DISABLE_COPY|Q_DISABLE_COPY_MOVE|QML_[A-Z_]+)\b")


def _declaration_lines(path: Path) -> Set[int]:
    """The lines of one file that are only a Qt declaration macro."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return set()
    return {number for number, line in enumerate(text.splitlines(), 1)
            if _DECLARATION_MACRO.match(line)}


def _collect(build_dir: Path, source_root: Path, gcov: str) -> Dict[Path, Tuple[Set[int], Set[int]]]:
    """Map each SynQt source file to (executable lines, executed lines). A header measured in
    several units counts as covered if any unit reached the line.
    """
    per_file: Dict[Path, Tuple[Set[int], Set[int]]] = {}
    gcda_files = sorted(build_dir.rglob("*.gcda"))
    if not gcda_files:
        raise SystemExit(
            "no .gcda counter files under %s: configure with -DSYNQT_COVERAGE=ON and run "
            "the suites before reporting" % build_dir)

    for document in _gcov_documents(gcov, gcda_files, build_dir):
        cwd = Path(document.get("current_working_directory") or build_dir)
        for entry in document.get("files", []):
            path = Path(entry.get("file", ""))
            if not path.is_absolute():
                path = cwd / path
            try:
                path = path.resolve()
            except OSError:
                continue
            if not path.is_relative_to(source_root):
                continue
            executable, executed = per_file.setdefault(path, (set(), set()))
            for line in entry.get("lines", []):
                number = line.get("line_number")
                if number is None:
                    continue
                executable.add(number)
                if line.get("count", 0) > 0:
                    executed.add(number)
    return per_file


def _wasm_only_lines(path: Path) -> Set[int]:
    """The lines of one file that only a WebAssembly build compiles: branches of
    `#if`/`#else`/`#endif` whose condition names Q_OS_WASM positively. Approximate; reported
    separately from the percentage.
    """
    branches: list = []
    wasm: Set[int] = set()
    for number, line in enumerate(path.read_text(encoding="utf-8",
                                                 errors="replace").splitlines(), 1):
        text = line.strip()
        if text.startswith("#if"):
            branches.append("Q_OS_WASM" in text
                            and "!defined" not in text
                            and not text.startswith("#ifndef"))
        elif text.startswith("#else") and branches:
            branches[-1] = not branches[-1]
        elif text.startswith("#endif") and branches:
            branches.pop()
        elif any(branches) and text and not text.startswith("//"):
            wasm.add(number)
    return wasm


def _unmeasured_wasm(source_root: Path, per_file: Dict[Path, Tuple[Set[int], Set[int]]]) -> int:
    """How many WebAssembly-only lines this build never compiled, across all of src/."""
    total = 0
    for path in sorted(source_root.rglob("*.cpp")):
        guarded = _wasm_only_lines(path)
        if not guarded:
            continue
        executable, _ = per_file.get(path.resolve(), (set(), set()))
        total += len(guarded - executable)
    return total


def _rows(per_file: Dict[Path, Tuple[Set[int], Set[int]]], source_root: Path) -> list:
    """One row per file that has an executable line of its own, in path order."""
    rows = []
    for path in sorted(per_file):
        executable, executed = per_file[path]
        declarations = _declaration_lines(path)
        executable = executable - declarations
        executed = executed - declarations
        if not executable:
            continue
        rows.append({
            "file": path.relative_to(source_root.parent).as_posix(),
            "lines": len(executable),
            "covered": len(executed),
            "percent": 100.0 * len(executed) / len(executable),
            "missing": sorted(executable - executed),
        })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build/coverage",
                        help="the instrumented build tree the suites ran in")
    parser.add_argument("--source-root", default="src",
                        help="only files under here are reported (default: src)")
    parser.add_argument("--fail-under", type=float, default=None,
                        help="exit non-zero when total line coverage is below this percent")
    parser.add_argument("--json", default=None, help="also write the figures here")
    parser.add_argument("--gcov", default=os.environ.get("GCOV", "gcov"),
                        help="the gcov to read the counters with (GCOV in the environment)")
    args = parser.parse_args()

    gcov = shutil.which(args.gcov)
    if gcov is None:
        raise SystemExit("no gcov on PATH (looked for %r); set GCOV to the one that "
                         "matches the compiler that built the tree" % args.gcov)

    build_dir = Path(args.build_dir).resolve()
    source_root = Path(args.source_root).resolve()
    if not build_dir.is_dir():
        raise SystemExit("no build tree at %s" % build_dir)

    per_file = _collect(build_dir, source_root, gcov)
    if not per_file:
        raise SystemExit("the counters name no file under %s" % source_root)

    rows = _rows(per_file, source_root)
    total_executable = sum(row["lines"] for row in rows)
    total_executed = sum(row["covered"] for row in rows)

    if not rows:
        raise SystemExit("the counters name no executable line under %s" % source_root)
    total = 100.0 * total_executed / total_executable

    width = max(len(row["file"]) for row in rows)
    print("%-*s  %7s %7s %8s" % (width, "file", "lines", "covered", "percent"))
    print("-" * (width + 26))
    for row in sorted(rows, key=lambda r: r["percent"]):
        print("%-*s  %7d %7d %7.1f%%" % (width, row["file"], row["lines"],
                                         row["covered"], row["percent"]))
    print("-" * (width + 26))
    print("%-*s  %7d %7d %7.1f%%" % (width, "TOTAL", total_executable, total_executed, total))

    unmeasured = _unmeasured_wasm(source_root, per_file)
    if unmeasured:
        print("\nnot in the figure above: %d line(s) behind #ifdef Q_OS_WASM, which a native "
              "build\ndoes not compile. Those run in a browser and are covered behaviourally "
              "by\nbrowser-matrix.yml, where no line counter follows them." % unmeasured)

    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps({
            "lines": total_executable,
            "covered": total_executed,
            "percent": round(total, 2),
            "wasm_only_lines_not_measured": unmeasured,
            "files": rows,
        }, indent=2) + "\n", encoding="utf-8")

    if args.fail_under is not None and total < args.fail_under:
        print("\nerror: C++ line coverage is %.1f%%, below the %.1f%% floor"
              % (total, args.fail_under), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
