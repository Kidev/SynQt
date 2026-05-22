#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Put the columns of the call-path comparison side by side.

A caller asks and waits for the answer: the shape of a Next.js Server Function and of a
returning slot.

    python3 benchmarks/vs-frameworks/compare-calls.py benchmarks/results/vs-call-*.json

Every column is printed.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Dict, List

from nodemajors import node_majors

# The table order: SynQt, the bare Node floor, then Next.js. `node-bare` separates the cost
# of an HTTP request from the cost of a server action.
NODE = node_majors()
STACK_ORDER = (["synqt"]
               + [f"node{major}-bare-call" for major in NODE]
               + [f"node{major}-nextjs-action" for major in NODE])


def load(paths: List[str]) -> Dict[str, Dict[str, Any]]:
    results: Dict[str, Dict[str, Any]] = {}
    for path in paths:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        results[data.get("stack", Path(path).stem)] = data
    return results


def rows_by_size(results: Dict[str, Dict[str, Any]]) -> Dict[int, Dict[str, Any]]:
    sizes: Dict[int, Dict[str, Any]] = {}
    for stack, data in results.items():
        for entry in data.get("sweep", []):
            sizes.setdefault(entry["callers"], {})[stack] = entry
    return dict(sorted(sizes.items()))


def marginal_rss(entries: List[Dict[str, Any]]) -> Dict[int, float]:
    """The memory cost of one more caller, from the slope between sizes. The smallest size has
    no slope.
    """
    ordered = sorted(entries, key=lambda e: e["callers"])
    slopes: Dict[int, float] = {}
    for previous, current in zip(ordered, ordered[1:]):
        delta_callers = current["callers"] - previous["callers"]
        delta_bytes = ((current.get("rss_total_bytes") or 0)
                       - (previous.get("rss_total_bytes") or 0))
        if delta_callers > 0 and delta_bytes > 0:
            slopes[current["callers"]] = delta_bytes / delta_callers
    return slopes


def calls_per_core_second(entry: Dict[str, Any]) -> str:
    """Calls one core sustains: 1e6 / `cpu_ms_per_1k` (CPU milliseconds per thousand calls)."""
    cpu_per_1k = entry.get("cpu_ms_per_1k") or 0
    if cpu_per_1k <= 0:
        return "-"
    return f"{1_000_000.0 / cpu_per_1k:,.0f}"


def main() -> int:
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        return 2
    results = load(paths)
    present = [s for s in STACK_ORDER if s in results] + \
              [s for s in results if s not in STACK_ORDER]
    if not present:
        print("no results to compare")
        return 2

    works = {results[stack].get("work", "?") for stack in present}
    if len(works) > 1:
        print(f"refusing to compare different workloads: {', '.join(sorted(works))}")
        return 2

    versions = []
    for stack in present:
        data = results[stack]
        versions.append(f"{stack}: " + (data.get("qt_version") and f"Qt {data['qt_version']}"
                                        or data.get("node_version", "?")))
    print("stacks   " + " | ".join(versions))
    print(f"host     {results[present[0]].get('host', '?')} "
          f"{results[present[0]].get('arch', '')}")
    print(f"work     {works.pop()}")
    print()

    slopes = {stack: marginal_rss(results[stack].get("sweep", [])) for stack in present}

    header = f"{'N':>6}  {'metric':<24}" + "".join(f"{s:>22}" for s in present)
    print(header)
    print("-" * len(header))

    for size, by_stack in rows_by_size(results).items():
        def line(label: str, render, stacks_render=None) -> None:
            cells = []
            for stack in present:
                if stacks_render is not None:
                    cells.append(f"{stacks_render(stack):>22}")
                elif stack in by_stack:
                    cells.append(f"{render(by_stack[stack]):>22}")
                else:
                    cells.append(f"{'-':>22}")
            print(f"{size:>6}  {label:<24}" + "".join(cells))

        line("latency p50 ms", lambda e: f"{e['latency']['p50']:.3f}")
        line("latency p99 ms", lambda e: f"{e['latency']['p99']:.3f}")
        line("throughput calls/s", lambda e: f"{e['throughput_calls_per_sec']:,.0f}")
        line("cpu ms / 1k calls", lambda e: f"{e['cpu_ms_per_1k']:.3f}")
        line("calls / core-second", calls_per_core_second)
        line("rss KiB / caller (marg)",
             lambda e: "-", stacks_render=lambda stack: (
                 f"{slopes[stack][size] / 1024:.1f}"
                 if size in slopes.get(stack, {}) else "-"))
        line("failed", lambda e: f"{e['failed']}")
        print()

    print("Reading it: latency is the call leaving to its answer being in hand, per call,")
    print("with exactly N calls outstanding. A failed count above zero means that stack")
    print("did not answer some calls, and every other number in its column is then a")
    print("figure over the ones it did.")
    print("The two Node columns hold an HTTP request per call and SynQt holds one open")
    print("WebSocket per caller; the README says what that does and does not license you")
    print("to conclude.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
