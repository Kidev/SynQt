#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Line coverage for the SynQt runtime libraries, read back from gcov.

`cmake -DSYNQT_COVERAGE=ON` instruments the five libraries under src/ and nothing else
(cmake/SynQtCoverage.cmake); running the suites leaves a .gcda counter file beside every
object file. This reads those, keeps the source files that are actually SynQt's, and
reports what fraction of their executable lines the suites reached.

It shells out to `gcov -t -j`, which prints one JSON document per .gcda instead of
scattering .gcov files through the build tree, and needs nothing installed beyond the
compiler that produced the counters. lcov and gcovr both do more than this; neither is a
dependency worth adding to read a number that gcov already knows.

    tools/coverage/report.py --build-dir build/coverage [--fail-under 70]

`--json` writes the same figures as a machine-readable file, which is what CI keeps.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Set, Tuple


def _gcov_documents(gcov: str, gcda_files, build_dir: Path):
    """Every gcov JSON document for these .gcda files, one batch of files per call.

    gcov takes many .gcda arguments at once and prints one document per line, so the
    process is started a handful of times rather than once per translation unit; a full
    tree has hundreds. The batch is bounded because a command line is.
    """
    batch = []
    for gcda in gcda_files:
        batch.append(str(gcda))
        if len(batch) >= 64:
            yield from _run_gcov(gcov, batch, build_dir)
            batch = []
    if batch:
        yield from _run_gcov(gcov, batch, build_dir)


def _run_gcov(gcov: str, batch, build_dir: Path):
    """Read one batch, and if gcov objects to any file in it, read them one at a time.

    gcov reports failure for the whole invocation when a single .gcda is unreadable (a
    stale one from an older build, a counter file a killed process never finished), and it
    is not worth trusting the partial output of a run that reported an error. Retrying
    singly costs one process per file in a batch that had a problem, and only then, and it
    is what keeps one bad counter file from turning the report into "no coverage at all"
    rather than into "one file short".
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


def _collect(build_dir: Path, source_root: Path, gcov: str) -> Dict[Path, Tuple[Set[int], Set[int]]]:
    """Map each SynQt source file to (executable lines, lines that were executed).

    A header included by several translation units is measured once per unit, so the same
    line arrives repeatedly with different counts. Covered wins over not covered: the
    question is whether the suites ever reached the line, not how many objects contain it.
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

    rows = []
    total_executable = 0
    total_executed = 0
    for path in sorted(per_file):
        executable, executed = per_file[path]
        total_executable += len(executable)
        total_executed += len(executed)
        rows.append({
            "file": str(path.relative_to(source_root.parent)),
            "lines": len(executable),
            "covered": len(executed),
            "percent": 100.0 * len(executed) / len(executable) if executable else 100.0,
            "missing": sorted(executable - executed),
        })

    total = 100.0 * total_executed / total_executable if total_executable else 100.0

    width = max(len(row["file"]) for row in rows)
    print("%-*s  %7s %7s %8s" % (width, "file", "lines", "covered", "percent"))
    print("-" * (width + 26))
    for row in sorted(rows, key=lambda r: r["percent"]):
        print("%-*s  %7d %7d %7.1f%%" % (width, row["file"], row["lines"],
                                         row["covered"], row["percent"]))
    print("-" * (width + 26))
    print("%-*s  %7d %7d %7.1f%%" % (width, "TOTAL", total_executable, total_executed, total))

    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps({
            "lines": total_executable,
            "covered": total_executed,
            "percent": round(total, 2),
            "files": rows,
        }, indent=2) + "\n", encoding="utf-8")

    if args.fail_under is not None and total < args.fail_under:
        print("\nerror: C++ line coverage is %.1f%%, below the %.1f%% floor"
              % (total, args.fail_under), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
