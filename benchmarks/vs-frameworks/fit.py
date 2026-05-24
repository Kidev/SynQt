#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Split each column's cost into a per-publish part and a per-subscriber part.

    cost = fixed + subscribers x marginal

fitted by least squares over the sweep in each result file:

    python3 benchmarks/vs-frameworks/fit.py benchmarks/results/vs-fw-*.json

Two quantities are fitted:

  propagation p50   what the median delivery waited (the README table's figure). The
                    slope is a per-subscriber cost as the middle subscriber sees it.
  cost per publish  subscribers / throughput: wall-clock time per publish. Meaningful
                    only on a saturating run; under pacing it reads back the pacing.

Give it a saturating sweep. A negative intercept means it was given paced data.

The payload mode separates a per-socket copy (grows with the payload) from a fixed
per-socket overhead (does not):

    python3 benchmarks/vs-frameworks/fit.py --payloads sweeps/payload
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

# The payload directory the payload mode walks. P<bytes>, one per size.
PAYLOAD_DIR = re.compile(r"^p(\d+)$")


class Fit:
    """One straight line through one column's sweep."""

    def __init__(self, fixed: float, marginal: float, quality: float, points: int) -> None:
        self.fixed = fixed
        self.marginal = marginal
        self.quality = quality
        self.points = points


def fit_line(xs: Sequence[float], ys: Sequence[float]) -> Fit | None:
    """Least squares through the sweep, with the coefficient of determination, which says how
    much of the split to believe.
    """
    count = len(xs)
    if count < 2:
        return None
    meanX = sum(xs) / count
    meanY = sum(ys) / count
    spread = sum((x - meanX) ** 2 for x in xs)
    if spread <= 0.0:
        return None
    marginal = sum((x - meanX) * (y - meanY) for x, y in zip(xs, ys)) / spread
    fixed = meanY - (marginal * meanX)
    total = sum((y - meanY) ** 2 for y in ys)
    residual = sum((y - (fixed + (marginal * x))) ** 2 for x, y in zip(xs, ys))
    quality = 1.0 if total <= 0.0 else 1.0 - (residual / total)
    return Fit(fixed, marginal, quality, count)


def sweep_points(data: Dict[str, Any], quantity: str) -> Tuple[List[float], List[float]]:
    """The (subscribers, microseconds) pairs one result file offers for one quantity."""
    xs: List[float] = []
    ys: List[float] = []
    for entry in data.get("sweep", []):
        subscribers = float(entry.get("subscribers", 0))
        if subscribers <= 0.0:
            continue
        if quantity == "p50":
            value = entry.get("propagation", {}).get("p50")
            if value is None:
                continue
            microseconds = float(value) * 1000.0
        else:
            throughput = float(entry.get("throughput_msgs_per_sec", 0.0))
            if throughput <= 0.0:
                continue
            # Deliveries per second over N subscribers is publishes per second; its
            # reciprocal is the time per publish.
            microseconds = subscribers / throughput * 1e6
        xs.append(subscribers)
        ys.append(microseconds)
    return xs, ys


def load(paths: Sequence[str]) -> Dict[str, Dict[str, Any]]:
    """Result files keyed by the stack that wrote them, newest wins on a repeat."""
    results: Dict[str, Dict[str, Any]] = {}
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        # The process sweep shares the prefix but not the shape; skip it.
        if data.get("benchmark") != "vs-frameworks-live":
            continue
        results[data.get("stack", Path(path).stem)] = data
    return results


def micro(value: float) -> str:
    """A microsecond figure at the precision it is worth, which is not four decimals."""
    if abs(value) >= 100.0:
        return f"{value:.0f}"
    if abs(value) >= 10.0:
        return f"{value:.1f}"
    return f"{value:.2f}"


def print_fits(results: Dict[str, Dict[str, Any]], quantity: str) -> None:
    label = "propagation p50" if quantity == "p50" else "cost per publish"
    print(f"{label}: fixed + subscribers x marginal, least squares")
    print()
    print(f"  {'column':<20} {'fixed/publish':>14} {'marginal/sub':>14} {'R2':>7}  sizes")
    saturated = True
    for stack, data in results.items():
        if not data.get("saturated", False):
            saturated = False
        xs, ys = sweep_points(data, quantity)
        fit = fit_line(xs, ys)
        if fit is None:
            print(f"  {stack:<20} {'too few sizes to fit':>37}")
            continue
        sizes = ",".join(str(int(x)) for x in xs)
        print(f"  {stack:<20} {micro(fit.fixed) + ' us':>14} "
              f"{micro(fit.marginal) + ' us':>14} {fit.quality:>7.3f}  {sizes}")
    print()
    if not saturated:
        print("  Note: at least one column was paced, not saturated. A paced run spends the")
        print("  interval idle between ticks, so its points do not lie on this line and its")
        print("  intercept is not a fixed cost. Rerun with --saturate.")
        print()


def print_payload_sweep(root: Path, quantity: str) -> int:
    """Refit at every payload size, so a copy can be told from a per-socket overhead."""
    sizes: List[Tuple[int, Dict[str, Dict[str, Any]]]] = []
    for child in sorted(root.iterdir()):
        matched = PAYLOAD_DIR.match(child.name)
        if not child.is_dir() or matched is None:
            continue
        files = sorted(str(path) for path in child.glob("vs-fw-*.json"))
        if files:
            sizes.append((int(matched.group(1)), load(files)))
    if not sizes:
        print(f"no p<bytes>/ result directories under {root}", file=sys.stderr)
        return 2
    sizes.sort(key=lambda item: item[0])

    stacks: List[str] = []
    for _, results in sizes:
        for stack in results:
            if stack not in stacks:
                stacks.append(stack)

    label = "propagation p50" if quantity == "p50" else "cost per publish"
    print(f"marginal cost per subscriber against payload size, fitted from {label}")
    print()
    header = "  " + f"{'column':<20}" + "".join(f"{str(size) + 'B':>11}" for size, _ in sizes)
    print(header)
    marginals: Dict[str, Dict[int, float]] = {}
    for stack in stacks:
        cells: List[str] = []
        for size, results in sizes:
            data = results.get(stack)
            fit = fit_line(*sweep_points(data, quantity)) if data else None
            if fit is None:
                cells.append(f"{'-':>11}")
                continue
            marginals.setdefault(stack, {})[size] = fit.marginal
            cells.append(f"{micro(fit.marginal):>11}")
        print(f"  {stack:<20}" + "".join(cells))
    print()
    print("  (microseconds per subscriber per publish)")
    print()

    print("  What the payload buys each stack, per subscriber:")
    print()
    # No line is fitted here: the cost is flat at small payloads and turns up at large ones.
    smallest = min(size for size, _ in sizes)
    largest = max(size for size, _ in sizes)
    print(f"  {'column':<20} {'at ' + str(smallest) + 'B':>11} {'at ' + str(largest) + 'B':>11} "
          f"{'growth':>8} {'knee':>9} {'per KiB above it':>18}")
    for stack in stacks:
        byPayload = marginals.get(stack, {})
        if smallest not in byPayload or largest not in byPayload:
            continue
        base = byPayload[smallest]
        top = byPayload[largest]
        # The knee: the first size whose marginal cost is a quarter above the smallest
        # payload's (run-to-run spread is a few percent).
        knee = next((size for size in sorted(byPayload)
                     if byPayload[size] > base * 1.25), None)
        # Read between the two largest sizes, where the payload is being paid for.
        ordered = sorted(byPayload)
        stride = ((byPayload[ordered[-1]] - byPayload[ordered[-2]])
                  / ((ordered[-1] - ordered[-2]) / 1024.0))
        print(f"  {stack:<20} {micro(base) + ' us':>11} {micro(top) + ' us':>11} "
              f"{top / base if base > 0 else 0:>7.1f}x "
              f"{(str(knee) + 'B') if knee else 'none':>9} {micro(stride) + ' us':>18}")
    print()
    print(f"  A column with no knee pays the same per subscriber whatever the frame carries,")
    print(f"  so its marginal cost is a syscall, a wakeup and a dispatch, not the bytes. One")
    print(f"  with a knee copies the payload per subscriber above that size, and the last")
    print(f"  column says what each further KiB then costs it.")
    print()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", nargs="*",
                        help="result files from one run of run-bench.sh")
    parser.add_argument("--payloads", metavar="DIR",
                        help="a directory of p<bytes>/ subdirectories, one sweep each")
    parser.add_argument("--quantity", choices=["p50", "publish"], default="p50",
                        help="what to fit: the median delivery's wait (the default, and "
                             "what the README quotes) or wall-clock time per publish")
    arguments = parser.parse_args()

    if arguments.payloads:
        return print_payload_sweep(Path(arguments.payloads), arguments.quantity)
    if not arguments.results:
        parser.error("give it result files, or --payloads with a directory of sweeps")
    print_fits(load(arguments.results), arguments.quantity)
    return 0


if __name__ == "__main__":
    sys.exit(main())
