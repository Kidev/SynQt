# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The measurement half shared by both Python columns, so only the server differs.

Matches ``node/measure.mjs``: the same distribution summary, frame layout (8 bytes of
microsecond stamp, then payload), delivered/expected pair and JSON. See
``benchmarks/vs-frameworks/COLUMN-CONTRACT.md``.
"""

from __future__ import annotations

import asyncio
import json
import os
import platform
import resource
import sys
import time
from datetime import datetime, timezone
from typing import Any, Callable, Dict, List, Sequence


def now_micros() -> int:
    """Microseconds since an arbitrary origin, on the monotonic clock
    (``time.perf_counter_ns``), so a clock step cannot enter the distribution.
    """
    return time.perf_counter_ns() // 1000


def make_frame(stamp_micros: int, payload: bytes) -> bytes:
    return stamp_micros.to_bytes(8, "little") + payload


def read_stamp(frame: bytes) -> int:
    return int.from_bytes(frame[:8], "little")


def resident_bytes() -> int:
    """Resident set size from ``/proc``, as every column reports it. ``ru_maxrss`` is the peak."""
    try:
        with open("/proc/self/statm", encoding="utf-8") as handle:
            pages = int(handle.read().split()[1])
        return pages * os.sysconf("SC_PAGE_SIZE")
    except (OSError, IndexError, ValueError):
        return 0


def cpu_milliseconds() -> float:
    """Process CPU in milliseconds, user plus system, as every column reports it."""
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return (usage.ru_utime + usage.ru_stime) * 1000


def distribution(samples: Sequence[float]) -> Dict[str, Any]:
    """Percentiles by linear interpolation between ranks, as ``measure.mjs`` computes them."""
    ordered = sorted(samples)

    def at(fraction: float) -> float:
        if not ordered:
            return 0.0
        rank = fraction * (len(ordered) - 1)
        low = int(rank)
        high = min(low + 1, len(ordered) - 1)
        if low == high:
            return ordered[low]
        return ordered[low] + (rank - low) * (ordered[high] - ordered[low])

    return {
        "unit": "ms",
        "samples": len(ordered),
        "min": ordered[0] if ordered else 0.0,
        "p50": at(0.5),
        "p95": at(0.95),
        "p99": at(0.99),
        "max": ordered[-1] if ordered else 0.0,
        "mean": sum(ordered) / len(ordered) if ordered else 0.0,
    }


def summarize(*, subscribers: int, propagation: Sequence[float], delivered: int,
              expected: int, elapsed_seconds: float, cpu_ms: float,
              rss_per_connection: float, rss_total: int) -> Dict[str, Any]:
    """One sweep entry, computed identically on both Python columns."""
    return {
        "subscribers": subscribers,
        "propagation": distribution(propagation),
        "throughput_msgs_per_sec": delivered / max(elapsed_seconds, 0.001),
        "cpu_ms_per_1k": (cpu_ms * 1000) / delivered if delivered else 0.0,
        "rss_bytes_per_conn": rss_per_connection,
        "rss_total_bytes": rss_total,
        "delivered": delivered,
        "expected": expected,
    }


def report(entry: Dict[str, Any]) -> None:
    spread = entry["propagation"]
    print(f"  N={entry['subscribers']}"
          f"  p50 {spread['p50']:.3f} ms"
          f"  p99 {spread['p99']:.3f} ms"
          f"  {entry['throughput_msgs_per_sec']:.0f} msg/s"
          f"  delivered {entry['delivered']}/{entry['expected']}", flush=True)


def parse_args(defaults: Dict[str, Any]) -> Dict[str, Any]:
    """``--flag value`` and bare ``--flag``, parsed as ``measure.mjs`` parses them."""
    values = dict(defaults)
    argv = sys.argv[1:]
    index = 0
    while index < len(argv):
        flag = argv[index]
        index += 1
        if not flag.startswith("--"):
            continue
        name = flag[2:]
        value = argv[index] if index < len(argv) else None
        if value is None or value.startswith("--"):
            values[name] = True
            continue
        values[name] = value
        index += 1
    return values


def write_result(path: str, root: Dict[str, Any]) -> Dict[str, Any]:
    try:
        with open("/proc/sys/kernel/osrelease", encoding="utf-8") as handle:
            release = handle.read().strip()
    except OSError:
        release = platform.release()
    complete = {
        "benchmark": "vs-frameworks-live",
        "python_version": platform.python_version(),
        # "linux <kernel release>", as measure.mjs writes it.
        "host": f"{sys.platform.rstrip('0123456789')} {release}",
        "arch": platform.machine(),
        "recorded": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "rss_available": True,
        **root,
    }
    if path:
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(complete, handle, indent=2)
            handle.write("\n")
        print(f"\nwrote {path}", flush=True)
    return complete


def sizes_of(value: Any) -> List[int]:
    return [size for size in
            (int(part.strip()) for part in str(value).split(",") if part.strip().isdigit())
            if size > 0]


async def drive(*, start_server: Callable[[], Any], connect: Callable[[str], Any],
                stack: str, path: str, args: Dict[str, Any]) -> Dict[str, Any]:
    """Run the whole sweep for one Python column.

    ``start_server`` returns an object with a ``url``, a ``broadcast(frame)`` coroutine, a
    ``connected()`` count and a ``close()`` coroutine. ``connect(url)`` returns a subscriber
    with ``recv()`` and ``close()``. Everything else is shared.
    """
    sizes = sizes_of(args["subscribers"])
    seconds = max(1, int(args["seconds"]))
    hz = max(1, int(args["hz"]))
    payload_bytes = max(0, int(args["payload"]))
    saturate = args["saturate"] is True or args["saturate"] == "true"

    print(f"{stack} live path: {seconds}s at "
          f"{'saturation' if saturate else f'{hz} Hz'}, {payload_bytes} byte payload, "
          f"subscribers {args['subscribers']}", flush=True)

    payload = b"x" * payload_bytes
    baseline_rss = resident_bytes()
    sweep: List[Dict[str, Any]] = []

    for subscriber_count in sizes:
        server = await start_server()
        state = {"measuring": False, "this_frame": 0}
        fleet = asyncio.Event()
        # One sample list per subscriber, appended by its own task.
        buffers: List[List[float]] = [[] for _ in range(subscriber_count)]
        subscribers = []
        tasks = []

        async def pump(index: int, socket: Any) -> None:
            # Kept minimal: server and clients share one event loop, so a slow callback
            # would delay the publisher.
            mine = buffers[index]
            while True:
                try:
                    frame = await socket.recv()
                except Exception:
                    return
                if not state["measuring"] or len(frame) < 8:
                    continue
                mine.append((now_micros() - read_stamp(frame)) / 1000)
                state["this_frame"] += 1
                if state["this_frame"] >= subscriber_count:
                    fleet.set()

        for index in range(subscriber_count):
            socket = await connect(server.url)
            subscribers.append(socket)
            tasks.append(asyncio.create_task(pump(index, socket)))

        deadline = time.monotonic() + 30
        while server.connected() < subscriber_count and time.monotonic() < deadline:
            await asyncio.sleep(0.01)
        if server.connected() < subscriber_count:
            print(f"subscribers did not come up at N={subscriber_count}", file=sys.stderr)
            raise SystemExit(1)
        connected_rss = resident_bytes()

        # Warm up before measuring; `measuring` is still false here.
        for _ in range(min(hz, 30)):
            await server.broadcast(make_frame(now_micros(), payload))
            await asyncio.sleep(1 / hz)
        state["measuring"] = True

        ticks = 0
        cpu_before = cpu_milliseconds()
        started_at = time.monotonic()
        if saturate:
            # Closed loop: publish, wait for every subscriber, publish again. The mode every
            # column shares.
            while time.monotonic() - started_at < seconds:
                state["this_frame"] = 0
                fleet.clear()
                await server.broadcast(make_frame(now_micros(), payload))
                ticks += 1
                try:
                    await asyncio.wait_for(fleet.wait(), timeout=5)
                except asyncio.TimeoutError:
                    print(f"a frame never reached every subscriber at N={subscriber_count}",
                          file=sys.stderr)
                    raise SystemExit(1)
        else:
            ticks = seconds * hz
            for tick in range(ticks):
                await server.broadcast(make_frame(now_micros(), payload))
                due = started_at + (tick + 1) / hz
                wait = due - time.monotonic()
                if wait > 0:
                    await asyncio.sleep(wait)
            # Let in-flight frames land before stopping.
            await asyncio.sleep(0.5)
        elapsed_seconds = time.monotonic() - started_at
        cpu_ms = cpu_milliseconds() - cpu_before
        state["measuring"] = False

        propagation = [sample for buffer in buffers for sample in buffer]
        entry = summarize(
            subscribers=subscriber_count,
            propagation=propagation,
            delivered=len(propagation),
            expected=ticks * subscriber_count,
            elapsed_seconds=elapsed_seconds,
            cpu_ms=cpu_ms,
            rss_per_connection=((connected_rss - baseline_rss) / subscriber_count
                                if subscriber_count and connected_rss > baseline_rss else 0),
            rss_total=connected_rss,
        )
        report(entry)
        sweep.append(entry)

        for task in tasks:
            task.cancel()
        for socket in subscribers:
            await socket.close()
        await server.close()
        await asyncio.sleep(0.1)

    return write_result(str(args["out"]), {
        "stack": stack,
        "path": path,
        "hz": 0 if saturate else hz,
        "saturated": saturate,
        "seconds": seconds,
        "payload_bytes": payload_bytes,
        "sweep": sweep,
    })


DEFAULTS: Dict[str, Any] = {
    "subscribers": "10,50,100,250",
    "seconds": "5",
    "hz": "30",
    "payload": "256",
    "saturate": False,
    "out": "",
}
