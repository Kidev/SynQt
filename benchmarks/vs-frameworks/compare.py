#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Put the columns of the live-path comparison side by side, and derive how many live users a
core and a gigabyte hold.

    python3 benchmarks/vs-frameworks/compare.py benchmarks/results/vs-fw-*.json

Every column is printed.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Dict, List

from nodemajors import node_majors

# The table order. SynQt, then `qt-raw` (the same fan-out without the object protocol), then
# the bare floor of each runtime, then the frameworks. The Next.js columns end the Node
# group: Next has no WebSocket server, so they use server-sent events. Each Node major gets
# its own column. A stack not listed here prints after these.
NODE = node_majors()
STACK_ORDER = (
    ["synqt", "qt-raw", "go-bare", "rust-bare"]
    + [f"node{major}-bare" for major in NODE]
    + ["phoenix", "dotnet-signalr"]
    + [f"node{major}-socketio" for major in NODE]
    + [f"node{major}-nextjs" for major in NODE]
    + ["ruby-actioncable", "php-reverb", "python-fastapi", "python-channels"]
)


def runtime_of(data: Dict[str, Any]) -> str:
    """Which runtime produced a column, from whichever `<runtime>_version` key it wrote."""
    if data.get("qt_version"):
        return f"Qt {data['qt_version']}"
    for key, value in data.items():
        if key.endswith("_version") and value:
            return f"{key[: -len('_version')]} {value}"
    return "?"


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
            sizes.setdefault(entry["subscribers"], {})[stack] = entry
    return dict(sorted(sizes.items()))


def marginal_rss(entries: List[Dict[str, Any]]) -> Dict[int, float]:
    """The memory cost of one more connection, from the slope between sizes. The per-connection
    ratio includes the runtime fixed cost at small N. The smallest size has no slope.
    """
    ordered = sorted(entries, key=lambda e: e["subscribers"])
    slopes: Dict[int, float] = {}
    for previous, current in zip(ordered, ordered[1:]):
        delta_conns = current["subscribers"] - previous["subscribers"]
        delta_bytes = (current.get("rss_total_bytes") or 0) - (previous.get("rss_total_bytes") or 0)
        if delta_conns > 0 and delta_bytes > 0:
            slopes[current["subscribers"]] = delta_bytes / delta_conns
    return slopes


def per_core_per_gb(entry: Dict[str, Any], marginal: float | None = None) -> str:
    """Live users one core and one gigabyte hold at this stack's measured cost. "-" where a
    measurement is missing. Memory uses the marginal cost when available.
    """
    cpu_per_1k = entry.get("cpu_ms_per_1k") or 0
    rss = marginal if marginal else (entry.get("rss_bytes_per_conn") or 0)
    throughput = entry.get("throughput_msgs_per_sec") or 0
    subscribers = entry.get("subscribers") or 1

    if cpu_per_1k <= 0 or throughput <= 0:
        by_cpu = "-"
    else:
        # A core sustains 1e6 / cpu_per_1k deliveries a second; each user costs this run's
        # delivery rate.
        deliveries_per_core_second = 1_000_000.0 / cpu_per_1k
        per_user_rate = throughput / subscribers
        by_cpu = f"{deliveries_per_core_second / max(per_user_rate, 1e-9):,.0f}"

    by_ram = f"{(1024 ** 3) / rss:,.0f}" if rss > 0 else "-"
    return f"{by_cpu} / {by_ram}"


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

    versions = [f"{stack}: {runtime_of(results[stack])}" for stack in present]
    print("stacks   " + " | ".join(versions))
    print(f"host     {results[present[0]].get('host', '?')} "
          f"{results[present[0]].get('arch', '')}")
    print()

    slopes = {stack: marginal_rss(results[stack].get("sweep", [])) for stack in present}

    header = f"{'N':>6}  {'metric':<22}" + "".join(f"{s:>18}" for s in present)
    print(header)
    print("-" * len(header))

    for size, by_stack in rows_by_size(results).items():
        def line(label: str, render, stacks_render=None) -> None:
            """One row. `render` reads a stack entry; `stacks_render` takes the stack name, for
            rows that span sizes.
            """
            cells = []
            for stack in present:
                if stacks_render is not None:
                    cells.append(f"{stacks_render(stack):>18}")
                elif stack in by_stack:
                    cells.append(f"{render(by_stack[stack]):>18}")
                else:
                    cells.append(f"{'-':>18}")
            print(f"{size:>6}  {label:<22}" + "".join(cells))

        line("propagation p50 ms", lambda e: f"{e['propagation']['p50']:.3f}")
        line("propagation p99 ms", lambda e: f"{e['propagation']['p99']:.3f}")
        line("throughput msg/s", lambda e: f"{e['throughput_msgs_per_sec']:,.0f}")
        line("cpu ms / 1k msgs", lambda e: f"{e['cpu_ms_per_1k']:.3f}")
        line("rss KiB / conn", lambda e: f"{e['rss_bytes_per_conn'] / 1024:.1f}")
        line("rss KiB / conn (marg)",
             lambda e, s=None: "-", stacks_render=lambda stack: (
                 f"{slopes[stack][size] / 1024:.1f}" if size in slopes.get(stack, {}) else "-"))
        line("delivered", lambda e: f"{e['delivered']}/{e['expected']}")
        line("users / core / GiB",
             lambda e, stack=None: per_core_per_gb(e, None),
             stacks_render=lambda stack: (
                 per_core_per_gb(by_stack[stack], slopes.get(stack, {}).get(size))
                 if stack in by_stack else "-"))
        print()

    print("Reading it: propagation is publisher push to subscriber handler, per delivery.")
    print("'users / core / GiB' is derived, and the two halves bind at different points:")
    print("whichever is smaller is the wall a deployment hits first.")
    print("A delivered count below expected means that stack dropped frames; every other")
    print("number in its column is then a figure over the survivors.")
    print("Prefer the marginal rss row: the plain one divides the process's fixed cost by")
    print("the connection count and so overstates small runs on every stack.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
