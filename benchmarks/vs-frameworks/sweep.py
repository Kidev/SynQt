#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Throughput against process count, for SynQt and for Node.

Both are single-threaded per process and scale by running more processes: SynQt with
`replicas:`, Node with `cluster`. (`threads:` is measured by `bench_live --threads N`; see
benchmarks/vs-frameworks/README.md.)

    python3 benchmarks/vs-frameworks/sweep.py --processes 1,2,4,8 --subscribers 200 --seconds 10

The subscriber count is fixed and split among the processes. Writes
`vs-fw-replicas-<host>.json`, which `benchmarks/baselines.py check` reads: throughput must
rise with process count, without dropped deliveries. No balancer: each process serves its
share directly.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

REPO_ROOT = Path(__file__).resolve().parents[2]
NODE_DIR = REPO_ROOT / "benchmarks" / "vs-frameworks" / "node"
DEFAULT_BINARY = REPO_ROOT / "build" / "bench-vs-frameworks" / "bench_live"


def node_binary() -> str:
    """The Node binary: the first major in node/runtimes.txt (the active LTS), resolved from
    nvm's version directories rather than PATH.
    """
    majors = [line.split("#", 1)[0].strip()
              for line in (NODE_DIR / "runtimes.txt").read_text(encoding="utf-8").splitlines()]
    majors = [m for m in majors if m]
    for major in majors:
        override = os.environ.get(f"SYNQT_NODE_{major}")
        if override and os.access(override, os.X_OK):
            return override
        root = Path(os.environ.get("NVM_DIR", Path.home() / ".nvm")) / "versions" / "node"
        installed = sorted(
            (d for d in root.glob(f"v{major}.*") if (d / "bin" / "node").is_file()),
            key=lambda d: [int(part) for part in d.name[1:].split(".")])
        if installed:
            return str(installed[-1] / "bin" / "node")
    found = shutil.which("node")
    if found:
        return found
    raise SystemExit(
        f"no Node found for any of {', '.join(majors) or '(none listed)'}; "
        f"nvm install {majors[0] if majors else '24'}, or set SYNQT_NODE_<major>")


def run_one(command: List[str], out_path: Path, cwd: Path) -> Dict[str, Any]:
    """Run one column at one process count and read back what it wrote."""
    subprocess.run(command + ["--out", str(out_path)], cwd=cwd, check=True,
                   stdout=subprocess.DEVNULL)
    return json.loads(out_path.read_text(encoding="utf-8"))


def total(document: Dict[str, Any]) -> Dict[str, float]:
    """Fold one process's sweep (a single size, here) into its totals."""
    rows = document.get("sweep", [])
    return {
        "throughput_msgs_per_sec": sum(r.get("throughput_msgs_per_sec", 0.0) for r in rows),
        "delivered": sum(r.get("delivered", 0) for r in rows),
        "expected": sum(r.get("expected", 0) for r in rows),
        "p50": max((r["propagation"]["p50"] for r in rows), default=0.0),
        "p99": max((r["propagation"]["p99"] for r in rows), default=0.0),
    }


def sweep_stack(name: str, command_for, counts: List[int], subscribers: int,
                scratch: Path, cwd: Path) -> List[Dict[str, Any]]:
    """One stack, swept over process count. The subscriber count is fixed and split; a
    remainder goes to the first processes.
    """
    rows: List[Dict[str, Any]] = []
    for count in counts:
        share, remainder = divmod(subscribers, count)
        if share == 0:
            print(f"  {name}: {count} processes is more than {subscribers} subscribers; "
                  f"skipping", file=sys.stderr)
            continue

        started: List[subprocess.Popen] = []
        outputs: List[Path] = []
        for index in range(count):
            mine = share + (1 if index < remainder else 0)
            out_path = scratch / f"{name}-{count}-{index}.json"
            outputs.append(out_path)
            started.append(subprocess.Popen(
                command_for(mine) + ["--out", str(out_path)],
                cwd=cwd, stdout=subprocess.DEVNULL))
        for process in started:
            process.wait()

        merged = {"throughput_msgs_per_sec": 0.0, "delivered": 0, "expected": 0,
                  "p50": 0.0, "p99": 0.0}
        for out_path in outputs:
            if not out_path.exists():
                print(f"  {name}: a process at count {count} wrote nothing", file=sys.stderr)
                return rows
            one = total(json.loads(out_path.read_text(encoding="utf-8")))
            merged["throughput_msgs_per_sec"] += one["throughput_msgs_per_sec"]
            merged["delivered"] += one["delivered"]
            merged["expected"] += one["expected"]
            # The fleet tail is the worst process's tail.
            merged["p50"] = max(merged["p50"], one["p50"])
            merged["p99"] = max(merged["p99"], one["p99"])

        row = {
            "count": count,
            "subscribers": subscribers,
            "throughput_msgs_per_sec": merged["throughput_msgs_per_sec"],
            "delivered": merged["delivered"],
            "expected": merged["expected"],
            "propagation": {"unit": "ms", "samples": merged["delivered"],
                            "min": 0.0, "p50": merged["p50"], "p95": merged["p99"],
                            "p99": merged["p99"], "max": merged["p99"],
                            "mean": merged["p50"]},
        }
        rows.append(row)
        print(f"  {name}: {count} process(es)  "
              f"{row['throughput_msgs_per_sec']:,.0f} msg/s  "
              f"worst p99 {merged['p99']:.3f} ms  "
              f"delivered {merged['delivered']}/{merged['expected']}")
        time.sleep(1)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--processes", default="1,2,4,8")
    parser.add_argument("--subscribers", type=int, default=200)
    parser.add_argument("--seconds", default="10")
    parser.add_argument("--binary", default=str(DEFAULT_BINARY))
    parser.add_argument("--out", default="")
    parser.add_argument("--scratch", default="")
    args = parser.parse_args()

    counts = [int(part) for part in args.processes.split(",") if part.strip()]
    binary = Path(args.binary)
    if not binary.exists():
        print(f"{binary} is not built; run benchmarks/vs-frameworks/run-bench.sh first",
              file=sys.stderr)
        return 2

    scratch = Path(args.scratch) if args.scratch else Path(
        os.environ.get("TMPDIR", "/tmp")) / "synqt-vs-frameworks-sweep"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)

    # Always saturating: at a fixed publish rate every process count reports that rate.
    common = ["--seconds", str(args.seconds), "--saturate"]
    print(f"process sweep: {args.subscribers} subscribers split across {args.processes} "
          f"process(es), {args.seconds}s at saturation")

    print("SynQt:")
    synqt = sweep_stack(
        "synqt", lambda n: [str(binary), "--subscribers", str(n)] + common,
        counts, args.subscribers, scratch, REPO_ROOT)

    node = node_binary()
    node_version = subprocess.run([node, "--version"], capture_output=True,
                                  text=True, check=False).stdout.strip()
    print(f"Node (bare), {node_version}:")
    node_sweep = sweep_stack(
        "node-bare", lambda n: [node, "live-bare.mjs", "--subscribers", str(n)] + common,
        counts, args.subscribers, scratch, NODE_DIR)

    host_tag = "".join(c if c.isalnum() or c in "_.-" else "_" for c in socket.gethostname())
    out_path = Path(args.out) if args.out else (
        REPO_ROOT / "benchmarks" / "results" / f"vs-fw-replicas-{host_tag}.json")

    document = {
        "benchmark": "vs-frameworks-replicas",
        "stack": "synqt",
        "qt_version": "6.12.0",
        "node_version": node_version,
        "host": f"{platform.system()} {platform.release()}",
        "arch": platform.machine(),
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "subscribers": args.subscribers,
        "seconds": int(args.seconds),
        "saturated": True,
        "processes": synqt,
        "node_processes": node_sweep,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(f"{json.dumps(document, indent=2)}\n", encoding="utf-8")
    print(f"\nwrote {out_path}")

    if synqt and node_sweep:
        print("\nSynQt against Node, by process count:")
        for left, right in zip(synqt, node_sweep):
            print(f"  {left['count']:>2} process(es): "
                  f"synqt {left['throughput_msgs_per_sec']:>10,.0f} msg/s   "
                  f"node {right['throughput_msgs_per_sec']:>10,.0f} msg/s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
