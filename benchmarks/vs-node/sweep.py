#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Throughput against process count, for SynQt and for Node.

This is the part of the comparison that is also the acceptance test for `replicas:`. Both
runtimes are single-threaded per process and reach the other cores the same way, by running
more of themselves: SynQt through `replicas:`, Node through `cluster`. So the fair question
is not "which is faster on one core" but "what does each do with four".

    python3 benchmarks/vs-node/sweep.py --processes 1,2,4,8 --subscribers 200 --seconds 10

It runs the same fixed workload at each process count, with the subscriber count held
constant and divided among the processes, and writes a `vs-node-replicas-<host>.json` that
`benchmarks/baselines.py check` reads: throughput must rise with process count, and no
process count may buy that throughput by dropping deliveries.

What it deliberately does NOT do is stand up a balancer. Each process serves its own share
of subscribers directly, which is what a balancer arranges anyway once the connection is
placed; adding nginx to the measurement would measure nginx. The claim under test is that N
SynQt edge processes do N processes' worth of work, and that is what this measures.
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
NODE_DIR = REPO_ROOT / "benchmarks" / "vs-node" / "node"
DEFAULT_BINARY = REPO_ROOT / "build" / "bench-vs-node" / "bench_live"


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
    """One stack, swept over process count.

    The subscriber count is held constant and split across the processes, because the
    question is what N processes do with one workload and not what N times the workload
    looks like. A count that does not divide evenly gives the remainder to the first
    processes, so nothing is silently dropped.
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
            # The worst process is the one a user notices, so the tail across a fleet is
            # the fleet's tail and not its average.
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
        print(f"{binary} is not built; run benchmarks/vs-node/run-bench.sh first",
              file=sys.stderr)
        return 2

    scratch = Path(args.scratch) if args.scratch else Path(
        os.environ.get("TMPDIR", "/tmp")) / "synqt-vs-node-sweep"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)

    # Saturating, always. At a fixed publish rate the throughput is the publish rate, so
    # every process count reports the same number and the sweep measures nothing: the first
    # run of this script reported 960 msg/s at 1, 2 and 4 processes alike. Capacity is what
    # scaling is about, and capacity is what a closed loop at maximum rate measures.
    common = ["--seconds", str(args.seconds), "--saturate"]
    print(f"process sweep: {args.subscribers} subscribers split across {args.processes} "
          f"process(es), {args.seconds}s at saturation")

    print("SynQt:")
    synqt = sweep_stack(
        "synqt", lambda n: [str(binary), "--subscribers", str(n)] + common,
        counts, args.subscribers, scratch, REPO_ROOT)

    print("Node (bare):")
    node = sweep_stack(
        "node-bare", lambda n: ["node", "live-bare.mjs", "--subscribers", str(n)] + common,
        counts, args.subscribers, scratch, NODE_DIR)

    host_tag = "".join(c if c.isalnum() or c in "_.-" else "_" for c in socket.gethostname())
    out_path = Path(args.out) if args.out else (
        REPO_ROOT / "benchmarks" / "results" / f"vs-node-replicas-{host_tag}.json")

    document = {
        "benchmark": "vs-node-replicas",
        "stack": "synqt",
        "qt_version": "6.11.1",
        "node_version": subprocess.run(["node", "--version"], capture_output=True,
                                       text=True, check=False).stdout.strip(),
        "host": f"{platform.system()} {platform.release()}",
        "arch": platform.machine(),
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "subscribers": args.subscribers,
        "seconds": int(args.seconds),
        "saturated": True,
        "processes": synqt,
        "node_processes": node,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(f"{json.dumps(document, indent=2)}\n", encoding="utf-8")
    print(f"\nwrote {out_path}")

    if synqt and node:
        print("\nSynQt against Node, by process count:")
        for left, right in zip(synqt, node):
            print(f"  {left['count']:>2} process(es): "
                  f"synqt {left['throughput_msgs_per_sec']:>10,.0f} msg/s   "
                  f"node {right['throughput_msgs_per_sec']:>10,.0f} msg/s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
