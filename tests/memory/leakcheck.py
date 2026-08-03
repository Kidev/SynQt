# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Ask the suites and benchmarks that already exist what they leave behind.

Two questions, because one tool cannot answer both.

`soak` runs a binary at two workloads and compares the peak resident set. Memory that is
still reachable is memory a leak checker will not report and this will: a container the
process keeps appending to is the shape of every leak this framework has actually had. The
answer is noisy by construction (the allocator keeps pages, caches warm up), so it is
printed for every binary and gated only where the growth is far past anything a healthy
suite measures.

`sanitize` reads the reports LeakSanitizer wrote while the same binaries ran and asks who
allocated what was lost. Precise where the other is broad, and blind where the other sees:
it reports only what is unreachable at exit. A leak record is charged to this repository
when a frame of ours appears near the top of its stack; one whose repository frame is
thirty frames down, under a font library loading its cache, is not ours no matter whose
main() is at the bottom.

Stdlib only, so this runs on whatever interpreter is on the machine.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# Where a repository frame has to appear in a leak's stack for the leak to be ours. Deeper
# than this and the allocation belongs to whatever library the frames above it name: every
# leak in a process has this repository's main() at the bottom of its stack, so "a frame of
# ours is present" charges us with the C library's font cache.
NEAR_FRAMES = 12

# How much a binary may grow per repetition of its whole workload before this is called a
# finding rather than a number. Deliberately loose: a suite repetition here builds and tears
# down entire QML engines, TLS servers and QtRO nodes, and Qt retains a little of each. It
# is a net for the case nobody thought to write a steady-state test for, not the gate;
# tests/memory/tst_memory.cpp is the gate, and it measures in bytes.
SOAK_LIMIT_KB_PER_RUN = 4096


def _run(command: Sequence[str], env: Optional[Dict[str, str]] = None) -> Tuple[int, int]:
    """Run command to completion; return (exit status, peak resident set in KB)."""
    pid = os.fork()
    if pid == 0: # child
        try:
            devnull = os.open(os.devnull, os.O_WRONLY)
            os.dup2(devnull, 1)
            os.dup2(devnull, 2)
            os.execve(command[0], list(command), env if env is not None else os.environ)
        finally:
            os._exit(127)
    _, status, usage = os.wait4(pid, 0)
    return status, usage.ru_maxrss


def _tests_of(build_dir: Path) -> List[Tuple[str, List[str]]]:
    """Every test ctest knows about in build_dir, as (name, command)."""
    import subprocess

    out = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build_dir,
                         capture_output=True, text=True, check=True).stdout
    tests = []
    for test in json.loads(out).get("tests", []):
        command = test.get("command") or []
        if command and Path(command[0]).exists():
            tests.append((test["name"], command))
    return tests


def soak(build_dir: Path, low: int, high: int, only: Optional[str]) -> int:
    """Run each suite at two repeat counts and report what it kept per repetition."""
    tests = _tests_of(build_dir)
    if only:
        tests = [t for t in tests if only in t[0]]
    if not tests:
        print("no tests found; build the tree first", file=sys.stderr)
        return 1

    findings = 0
    unrepeatable: List[str] = []
    print(f"{'suite':<24} {'x' + str(low):>10} {'x' + str(high):>10} {'KB/run':>10}")
    for name, command in sorted(tests):
        low_status, low_rss = _run([*command, "-repeat", str(low), "-silent"])
        high_status, high_rss = _run([*command, "-repeat", str(high), "-silent"])
        if low_status != 0 or high_status != 0:
            # A suite that fails when run twice in one process is not a memory result: it
            # is a suite whose fixture does not survive its own second run (a table it
            # creates, a port it holds). Named rather than dropped, since a suite silently
            # missing from this table would read as one that passed it.
            unrepeatable.append(name)
            continue
        per_run = (high_rss - low_rss) / (high - low)
        flag = "  <-- grows" if per_run > SOAK_LIMIT_KB_PER_RUN else ""
        print(f"{name:<24} {low_rss:>10} {high_rss:>10} {per_run:>10.0f}{flag}")
        if per_run > SOAK_LIMIT_KB_PER_RUN:
            findings += 1
    if unrepeatable:
        print(f"\nnot measured, will not run twice in one process: {', '.join(unrepeatable)}")
    return 1 if findings else 0


_LEAK_HEAD = re.compile(r"^(Direct|Indirect) leak of (\d+) byte")


def _records(log_dir: Path, repo: Path) -> List[dict]:
    """Every leak record in every LeakSanitizer log under log_dir."""
    here = str(repo.resolve())
    records = []
    for log in sorted(log_dir.glob("asan.*")):
        text = log.read_text(encoding="utf-8", errors="replace")
        for block in re.split(r"\n(?=(?:Direct|Indirect) leak of )", text):
            head = _LEAK_HEAD.match(block)
            if not head:
                continue
            frames = [line.strip() for line in block.splitlines()[1:]
                      if line.strip().startswith("#")]
            depth = None
            where = ""
            for index, frame in enumerate(frames):
                # A build directory holds generated code; it is ours, but naming the source
                # it was generated from is not something a stack can do, so it is reported
                # by the path it has.
                if here in frame:
                    depth = index
                    where = frame[frame.index(here) + len(here) + 1:].split()[0]
                    break
            records.append({
                "log": log.name,
                "kind": head.group(1),
                "bytes": int(head.group(2)),
                "depth": depth,
                "where": where,
            })
    return records


def sanitize(log_dir: Path, repo: Path) -> int:
    """Report what LeakSanitizer lost, charged to whoever allocated it."""
    records = _records(log_dir, repo)
    if not records:
        print("no leak reports: every binary exited clean")
        return 0

    # Direct records only. An indirect record is a block reachable from another leaked
    # block, so it names a child, not a culprit: the QSslServer a leaked edge owns is
    # allocated in src/ and lost because a test never freed the edge. Charging those to the
    # framework would report one leak as a hundred and point at the wrong file for all of
    # them. The root of every one of them is a direct record, which is what is read here.
    direct = [r for r in records if r["kind"] == "Direct"]
    ours = [r for r in direct if r["depth"] is not None and r["depth"] <= NEAR_FRAMES]
    framework = [r for r in ours if r["where"].startswith("src/")]
    suites = [r for r in ours if not r["where"].startswith("src/")]
    upstream = len(direct) - len(ours)

    def summarize(title: str, group: List[dict]) -> None:
        print(f"\n{title}: {len(group)} records, {sum(r['bytes'] for r in group)} bytes")
        counts: Dict[str, List[int]] = {}
        for record in group:
            counts.setdefault(record["where"], [0, 0])
            counts[record["where"]][0] += 1
            counts[record["where"]][1] += record["bytes"]
        for where, (count, size) in sorted(counts.items(), key=lambda kv: -kv[1][1]):
            print(f"  {count:>5} records {size:>9} bytes  {where}")

    summarize("framework (src/), which is what this gate is for", framework)
    summarize("suites and benchmarks (a fixture the test never freed)", suites)
    print(f"\nupstream (no frame of ours within {NEAR_FRAMES} of the allocation): "
          f"{upstream} roots")

    # Reported, never gated. An indirect record is a child of a leaked root, and a root can
    # be a region LeakSanitizer scanned conservatively, which is how an object of ours ends
    # up filed under somebody else's arena. Read as evidence: an allocation of ours here is
    # a pointer someone dropped, and worth looking at even though it is not proof.
    indirect = [r for r in records
                if r["kind"] != "Direct" and r["depth"] is not None
                and r["depth"] <= NEAR_FRAMES]
    summarize("held by a leaked root, allocated by us (evidence, not a verdict)", indirect)
    return 1 if framework else 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    soak_parser = sub.add_parser("soak", help="run each suite twice and compare peak RSS")
    soak_parser.add_argument("build_dir", type=Path)
    soak_parser.add_argument("--low", type=int, default=2)
    soak_parser.add_argument("--high", type=int, default=6)
    soak_parser.add_argument("--only", default=None, help="only suites whose name holds this")

    sanitize_parser = sub.add_parser("sanitize", help="classify LeakSanitizer reports")
    sanitize_parser.add_argument("log_dir", type=Path)
    sanitize_parser.add_argument("--repo", type=Path,
                                 default=Path(__file__).resolve().parents[2])

    args = parser.parse_args(argv)
    if args.command == "soak":
        return soak(args.build_dir, args.low, args.high, args.only)
    return sanitize(args.log_dir, args.repo)


if __name__ == "__main__":
    sys.exit(main())
