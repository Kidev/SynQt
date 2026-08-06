# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Ask the existing suites and benchmarks what they leave behind.

`soak` runs a binary at two workloads and compares peak resident set, which catches memory
that is still reachable. It is noisy, so it is printed for every binary and gated only far
past healthy growth.

`sanitize` reads the LeakSanitizer reports from the same runs. A record is charged to this
repository when a repository frame is near the top of its stack. A leaked graph whose
members all point at each other has no direct record; such processes are listed with what
they lost, and `soak` gates them.

Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# How near the top of a leak's stack a repository frame must be for the leak to count. Every
# stack has the repository main() at the bottom.
NEAR_FRAMES = 12

# Frames that stop the search: a signal dispatch changes author, so code above it belongs to
# the slot, not the emitter (QtRO builds a dynamic Replica metaobject in onClientRead, above
# an `emit`).
DISPATCH_FRAMES = ("doActivate", "QSlotObjectBase::call", "QMetaObject::activate",
                   "QMetaMethod::invoke", "QMetaObject::invokeMethod")

# Allowed growth per workload repetition before it is a finding. Loose: a repetition builds
# and tears down QML engines, TLS servers and QtRO nodes. tests/memory/tst_memory.cpp is the
# byte-level gate.
SOAK_LIMIT_KB_PER_RUN = 4096


def _run(command: Sequence[str], env: Optional[Dict[str, str]] = None,
         cwd: Optional[str] = None) -> Tuple[int, int]:
    """Run command to completion and return (exit status, peak resident set in KB)."""
    pid = os.fork()
    if pid == 0: # child
        try:
            devnull = os.open(os.devnull, os.O_WRONLY)
            os.dup2(devnull, 1)
            os.dup2(devnull, 2)
            if cwd:
                os.chdir(cwd)
            os.execve(command[0], list(command), env if env is not None else os.environ)
        finally:
            os._exit(127)
    _, status, usage = os.wait4(pid, 0)
    return status, usage.ru_maxrss


class Suite:
    """One test as ctest would run it: its command and the ENVIRONMENT and WORKING_DIRECTORY
    its CMakeLists gives it.
    """

    def __init__(self, name: str, command: List[str], env: Dict[str, str],
                 cwd: Optional[str]) -> None:
        self.name = name
        self.command = command
        self.env = env
        self.cwd = cwd


def _tests_of(build_dir: Path) -> List[Suite]:
    """Every test ctest knows about in build_dir."""
    import subprocess

    out = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build_dir,
                         capture_output=True, text=True, check=True).stdout
    tests = []
    for test in json.loads(out).get("tests", []):
        command = test.get("command") or []
        if not command or not Path(command[0]).exists():
            continue
        env = dict(os.environ)
        cwd = None
        for prop in test.get("properties", []):
            if prop.get("name") == "ENVIRONMENT":
                for assignment in prop.get("value", []):
                    key, _, value = assignment.partition("=")
                    env[key] = value
            elif prop.get("name") == "WORKING_DIRECTORY":
                cwd = prop.get("value")
        tests.append(Suite(test["name"], command, env, cwd))
    return tests


def soak(build_dir: Path, low: int, high: int, only: Optional[str]) -> int:
    """Run each suite at two repeat counts and report what it kept per repetition."""
    tests = _tests_of(build_dir)
    # tst_memory is the sibling gate and measures its own drift; it is not soaked.
    tests = [t for t in tests if t.name != "memory"]
    if only:
        tests = [t for t in tests if only in t.name]
    if not tests:
        print("no tests found; build the tree first", file=sys.stderr)
        return 1

    findings = 0
    unrepeatable: List[str] = []
    print(f"{'suite':<24} {'x' + str(low):>10} {'x' + str(high):>10} {'KB/run':>10}")
    for suite in sorted(tests, key=lambda t: t.name):
        name = suite.name
        low_status, low_rss = _run([*suite.command, "-repeat", str(low), "-silent"],
                                   suite.env, suite.cwd)
        high_status, high_rss = _run([*suite.command, "-repeat", str(high), "-silent"],
                                     suite.env, suite.cwd)
        if low_status != 0 or high_status != 0:
            # A suite that fails when run twice is reported by name, not as a memory result.
            unrepeatable.append(name)
            continue
        per_run = (high_rss - low_rss) / (high - low)
        note = ""
        if per_run > SOAK_LIMIT_KB_PER_RUN:
            # Over the limit, the suite is asked again from `high` to twice as deep; only a
            # slope that holds there is a finding (a high-water mark divides down).
            deeper = high * 2 + low
            deep_status, deep_rss = _run([*suite.command, "-repeat", str(deeper), "-silent"],
                                         suite.env, suite.cwd)
            if deep_status != 0:
                unrepeatable.append(name)
                continue
            deep_per_run = (deep_rss - high_rss) / (deeper - high)
            if deep_per_run > SOAK_LIMIT_KB_PER_RUN:
                note = f"  <-- grows (x{deeper}: {deep_per_run:.0f} KB/run)"
                findings += 1
            else:
                note = f"  (settles: x{deeper}: {deep_per_run:.0f} KB/run)"
        print(f"{name:<24} {low_rss:>10} {high_rss:>10} {per_run:>10.0f}{note}")
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
                # Generated code in a build directory is reported by its path.
                if here in frame:
                    depth = index
                    where = frame[frame.index(here) + len(here) + 1:].split()[0]
                    break
                if any(marker in frame for marker in DISPATCH_FRAMES):
                    break   # upstream reacting to a repository event, see DISPATCH_FRAMES
            # Name the suite by the deepest test path in the whole block.
            suite = ""
            for frame in reversed(frames):
                marker = here + "/tests/"
                if marker in frame:
                    suite = frame[frame.index(marker) + len(here) + 1:].split()[0]
                    break
            records.append({
                "log": log.name,
                "kind": head.group(1),
                "bytes": int(head.group(2)),
                "depth": depth,
                "where": where,
                "suite": suite,
            })
    return records


def sanitize(log_dir: Path, repo: Path) -> int:
    """Report what LeakSanitizer lost, charged to whoever allocated it."""
    records = _records(log_dir, repo)
    if not records:
        print("no leak reports: every binary exited clean")
        return 0

    # Read each process on its own; the two leaked-graph shapes need different answers.
    by_log: Dict[str, List[dict]] = {}
    for record in records:
        by_log.setdefault(record["log"], []).append(record)

    direct: List[dict] = []
    children: List[dict] = []
    rootless: List[Tuple[str, int, int]] = []
    for log in sorted(by_log):
        group = by_log[log]
        roots = [r for r in group if r["kind"] == "Direct"]
        rest = [r for r in group if r["kind"] != "Direct"]
        if roots:
            direct.extend(roots)
            children.extend(rest)
            continue
        named = [r["suite"] for r in rest if r["suite"]]
        label = max(set(named), key=named.count).split(":")[0] if named else log
        rootless.append((label, len(rest), sum(r["bytes"] for r in rest)))

    # Direct records only. An indirect record is a child of a leaked block and would charge
    # one leak many times to the wrong file.
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
    print(f"\nupstream (no frame of ours within {NEAR_FRAMES} of the allocation, or only "
          f"below a signal dispatch): {upstream} roots")

    if rootless:
        # A process with no direct record: every leaked block is pointed at by another (a
        # QObject tree, or any cycle). It is reported, not gated per site, since any member
        # can appear at any stack depth. The soak pass gates this shape.
        print("\nleaked whole, so LeakSanitizer named no root and no site here can be "
              f"charged ({len(rootless)} processes); the soak pass is what gates these:")
        for label, count, size in sorted(rootless, key=lambda entry: -entry[2]):
            print(f"  {count:>5} records {size:>9} bytes  {label}")

    # Reported, never gated. An indirect record may sit under a conservatively scanned
    # region.
    evidence = [r for r in children
                if r["depth"] is not None and r["depth"] <= NEAR_FRAMES]
    summarize("held by a leaked root, allocated by us (evidence, not a verdict)", evidence)
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
