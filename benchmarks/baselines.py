# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Guard the benchmark baselines: validate one, or diff two.

Every harness under `benchmarks/` writes a JSON result and commits it under
`results/` as a baseline. Until now nothing checked those files, which left two
different gaps, and they want two different answers.

*The absolute numbers only mean something on one machine.* A p50 of 23 microseconds is
a fact about the author's workstation. Comparing it against a shared CI runner, which
is virtualised, noisy, and a different CPU, would produce a gate that fails for reasons
that have nothing to do with the commit under review. So absolute comparison lives in
`compare`, is opt-in, and is meant to be run twice on the *same* runner.

*The claims the numbers support are machine-independent, and those can be gated
anywhere.* "Interest management holds the per-session payload flat", "minting a session
is amortized O(1)", "the contended writer never approaches the busy timeout", "one-way
propagation is cheaper than a round trip": none of these is a statement about clock
speed. They are ratios, orderings, and invariants, and if one of them breaks, the
benchmark story in `README.md` has become false regardless of the hardware. That is
what `check` enforces, on a committed baseline or on a fresh run.

One rule governs which claims are asserted. **A claim is asserted only when the
committed baseline clears it with at least a 2x margin**; everything tighter is
reported and not enforced. Local-socket throughput beats mutual TLS by 1.2x, for
instance, which is real but well inside the noise of a shared runner, so it prints and
never fails. A gate that flaps gets disabled, and a disabled gate guards nothing.

Usage:

    python benchmarks/baselines.py check                 # every committed baseline
    python benchmarks/baselines.py check fresh.json      # a run that just finished
    python benchmarks/baselines.py compare old.json new.json --tolerance 0.25
    python benchmarks/baselines.py show results/mesh-kidevPC_.json
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Mapping, Optional, Sequence

BENCHMARKS_DIR = Path(__file__).resolve().parent
RESULTS_DIR = BENCHMARKS_DIR / "results"

# The margin a claim has to clear in the committed baseline before it is worth asserting.
# See the module docstring: anything tighter is reported instead.
ASSERT_MARGIN = 2.0

# Units where a smaller number is the better one. Anything else (calls/s, rows/s,
# requests/s, fps) reads the other way, and `compare` needs to know which.
LOWER_IS_BETTER_UNITS = {"ms", "ns", "ns/op", "us", "s", "bytes", "mb"}


class BaselineError(Exception):
    """A malformed result file: not a regression, a file that cannot be read as one."""


@dataclass(frozen=True)
class Metric:
    """One comparable number, flattened out of whatever shape its harness writes."""

    key: str
    value: float
    unit: str
    lower_is_better: bool
    #: One of NOISY_FIELDS: diffed and printed, but not gated unless `compare --strict`.
    noisy: bool = False


@dataclass
class Check:
    """One invariant, with the numbers that decided it (so a failure explains itself)."""

    name: str
    ok: bool
    detail: str
    enforced: bool = True

    @property
    def status(self) -> str:
        if self.ok:
            return "ok"
        return "FAIL" if self.enforced else "note"


@dataclass
class Report:
    """The outcome of checking one file."""

    path: Path
    kind: str
    checks: List[Check] = field(default_factory=list)

    @property
    def failures(self) -> List[Check]:
        return [c for c in self.checks if c.enforced and not c.ok]

    @property
    def ok(self) -> bool:
        return not self.failures


# Reading


def load(path: Path) -> Dict[str, Any]:
    """Read a result file, failing with the path in the message rather than a traceback."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise BaselineError(f"{path}: cannot be read ({error})") from error
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise BaselineError(f"{path}: not valid JSON ({error})") from error
    if not isinstance(document, dict):
        raise BaselineError(f"{path}: expected a JSON object at the top level")
    return document


def kind_of(document: Mapping[str, Any], path: Optional[Path] = None) -> str:
    """Name the harness that wrote a document.

    Most harnesses stamp `benchmark`. `measure-bundle.sh` predates that and writes only
    the weights, so it is recognised by shape, and the remote-pages baseline is
    recognised by its two-variant structure.
    """
    named = document.get("benchmark")
    if isinstance(named, str) and named:
        return named
    if "total_raw" in document and "files" in document:
        return "client-bundle"
    if "remote" in document and "compiled_in" in document:
        return "remote-pages"
    where = f"{path}: " if path else ""
    raise BaselineError(f"{where}cannot tell which benchmark wrote this (no `benchmark` key)")


def committed_baselines() -> List[Path]:
    """Every baseline the repository stores, in a stable order."""
    paths = sorted(RESULTS_DIR.glob("*.json"))
    remote_pages = BENCHMARKS_DIR / "remote-pages" / "baseline.json"
    if remote_pages.exists():
        paths.append(remote_pages)
    return paths


# Flattening: every shape down to one dictionary of comparable numbers

DISTRIBUTION_FIELDS = ("p50", "p95", "p99", "mean", "min", "max")

#: Diffed and printed, but not gated: these move for reasons that are not the code.
#:
#: `mean` is in here for a reason that only showed up once the comparison was run against
#: real data. The transport harness carries a single ~40 ms outlier in
#: `property_push_propagation` (first sample, before the link warms); it sits in the
#: committed baseline and in every fresh run alike. With 3000 samples it lifts the mean a
#: little, with 1000 it lifts it three times as much, so a shorter run "regressed" the
#: mean by 89% while every percentile *improved* by 12%. One sample can do that to a mean
#: and cannot do it to a median. p50 is the robust central estimate and is what the gate
#: reads.
NOISY_FIELDS = ("p95", "p99", "max", "mean")


def _lower_is_better(unit: str) -> bool:
    return unit.lower() in LOWER_IS_BETTER_UNITS


def _flatten_distribution(prefix: str, block: Mapping[str, Any], out: Dict[str, Metric]) -> None:
    unit = str(block.get("unit", "ms"))
    for field_name in DISTRIBUTION_FIELDS:
        if field_name not in block:
            continue
        key = f"{prefix}.{field_name}"
        out[key] = Metric(
            key=key,
            value=float(block[field_name]),
            unit=unit,
            lower_is_better=_lower_is_better(unit),
            noisy=field_name in NOISY_FIELDS,
        )


def flatten(document: Mapping[str, Any]) -> Dict[str, Metric]:
    """Turn any result document into `key -> Metric`, so two runs can be diffed.

    The keys are stable and readable (`mtls_loopback.connection_setup.p50`,
    `sweep.100.publish_cpu.p50`) because they are what a regression report shows a human.
    """
    metrics: Dict[str, Metric] = {}
    kind = kind_of(document)

    for block in document.get("latency", []):
        _flatten_distribution(str(block.get("name", "unnamed")), block, metrics)

    for block in list(document.get("throughput", [])) + list(document.get("scalars", [])):
        name = str(block.get("name", "unnamed"))
        unit = str(block.get("unit", ""))
        metrics[name] = Metric(name, float(block["value"]), unit, _lower_is_better(unit))

    for row in document.get("model_replication", []):
        key = f"model_replication.{row['rows']}_rows"
        metrics[key] = Metric(key, float(row["ms"]), "ms", True)

    for row in document.get("sweep", []):
        label = _sweep_label(kind, row)
        for name, value in row.items():
            if isinstance(value, Mapping):
                _flatten_distribution(f"{label}.{name}", value, metrics)
            elif isinstance(value, (int, float)) and not isinstance(value, bool):
                key = f"{label}.{name}"
                unit = _sweep_unit(name)
                metrics[key] = Metric(key, float(value), unit, _lower_is_better(unit))

    for row in document.get("results", []):
        label = f"{row.get('test', 'unnamed')}.{row.get('connections', 0)}c"
        metrics[f"{label}.requests_per_sec"] = Metric(
            f"{label}.requests_per_sec", float(row["requests_per_sec"]), "req/s", False
        )
        latency = row.get("latency_ms", {})
        for field_name in ("p50", "p90", "p99", "mean", "max"):
            if field_name in latency:
                key = f"{label}.latency.{field_name}"
                metrics[key] = Metric(
                    key, float(latency[field_name]), "ms", True, field_name in NOISY_FIELDS
                )

    for row in document.get("buckets", []):
        label = f"blobs_{row['blobs']}"
        for field_name in ("p50", "p95", "p99", "mean"):
            if field_name in row:
                key = f"{label}.frame_{field_name}"
                metrics[key] = Metric(
                    key, float(row[field_name]), "ms", True, field_name in NOISY_FIELDS
                )

    for name in ("total_raw", "total_gzip", "total_brotli", "cold_start_ms"):
        if name in document:
            unit = "ms" if name.endswith("_ms") else "bytes"
            metrics[name] = Metric(name, float(document[name]), unit, True)

    for variant in ("remote", "compiled_in"):
        block = document.get(variant)
        if isinstance(block, Mapping):
            for name in ("total_raw", "total_gzip", "total_brotli"):
                key = f"{variant}.{name}"
                metrics[key] = Metric(key, float(block[name]), "bytes", True)

    return metrics


def _sweep_label(kind: str, row: Mapping[str, Any]) -> str:
    """Name a sweep row by what varies in it, not by its index.

    An index would silently re-point every key the moment a sweep gains a size, which is
    exactly when a comparison matters most.
    """
    if "mode" in row and "consumers" in row:
        return f"{row['mode']}.n{row['consumers']}"
    for axis in ("players", "sessions", "consumers", "blobs", "target"):
        if axis in row:
            return f"{axis}_{row[axis]}"
    return "sweep"


def _sweep_unit(name: str) -> str:
    if name.endswith("_ns"):
        return "ns"
    if name.endswith("_ms"):
        return "ms"
    if name.endswith("_hz"):
        return "hz"
    if name.endswith("_mb"):
        return "mb"
    if name.endswith("_s"):
        return "s"
    return ""


# The universal checks: is this a well-formed result at all

REQUIRED_METADATA = ("host", "qt_version", "recorded")

#: `recorded` may say this, and only this, instead of a timestamp. Two client baselines
#: predate the metadata stamp and arrived in one squashed import commit, so their date is
#: genuinely not recoverable; writing a plausible one would turn a guess into a record.
#: Saying so keeps the gap visible on every run instead of hiding it behind a missing key.
UNKNOWN = "unknown"


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _check_metadata(document: Mapping[str, Any], kind: str, checks: List[Check]) -> None:
    """A baseline nobody can attribute is not a baseline.

    Which machine, which Qt, and when: without all three a later run has nothing to
    compare itself against, and the number is only a number.
    """
    missing = [key for key in REQUIRED_METADATA if not document.get(key)]
    checks.append(
        Check(
            "metadata",
            not missing,
            "host, qt_version and recorded are present"
            if not missing
            else f"missing: {', '.join(missing)}",
        )
    )
    if document.get("recorded") == UNKNOWN:
        checks.append(
            Check(
                "metadata.undated",
                True,
                "predates the metadata stamp and cannot be dated; re-run to replace it",
                enforced=False,
            )
        )


def _check_distributions(document: Mapping[str, Any], checks: List[Check]) -> None:
    """Percentiles that do not increase, or a mean outside its own range, mean the
    harness computed them wrongly; every number downstream is then untrustworthy."""
    broken: List[str] = []
    empty: List[str] = []
    for name, block in _iter_distributions(document):
        ordered = [block[f] for f in ("min", "p50", "p95", "p99", "max") if f in block]
        if not all(_finite(v) for v in ordered):
            broken.append(f"{name} (not finite)")
            continue
        if any(later < earlier for earlier, later in zip(ordered, ordered[1:])):
            broken.append(f"{name} ({' > '.join(f'{v:g}' for v in ordered)})")
        if "mean" in block and _finite(block["mean"]):
            if not (block.get("min", 0) <= block["mean"] <= block.get("max", math.inf)):
                broken.append(f"{name} (mean {block['mean']:g} outside min..max)")
        samples = block.get("samples")
        if samples is not None and samples <= 0:
            empty.append(name)
    counted = _count_distributions(document)
    if broken:
        detail = "; ".join(broken)
    elif counted:
        detail = f"{counted} distributions ordered min <= p50 <= p95 <= p99 <= max"
    else:
        detail = "no distributions to order (this harness reports per-operation costs)"
    checks.append(Check("distributions", not broken, detail))
    if empty:
        checks.append(Check("samples", False, f"measured from zero samples: {', '.join(empty)}"))


def _iter_distributions(document: Mapping[str, Any]) -> Iterable[tuple]:
    for block in document.get("latency", []):
        yield str(block.get("name", "unnamed")), block
    for row in document.get("sweep", []):
        label = _sweep_label(kind_of(document), row)
        for name, value in row.items():
            if isinstance(value, Mapping):
                yield f"{label}.{name}", value
    for row in document.get("results", []):
        if isinstance(row.get("latency_ms"), Mapping):
            yield f"{row.get('test')}.{row.get('connections')}c", row["latency_ms"]
    for row in document.get("buckets", []):
        yield f"blobs_{row.get('blobs')}", row


def _count_distributions(document: Mapping[str, Any]) -> int:
    return sum(1 for _ in _iter_distributions(document))


# The per-benchmark invariants: the claims README.md makes, mechanically


def _ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else math.inf


def _by_name(rows: Sequence[Mapping[str, Any]], name: str) -> Optional[Mapping[str, Any]]:
    for row in rows:
        if row.get("name") == name:
            return row
    return None


def _check_transport(document: Mapping[str, Any], checks: List[Check]) -> None:
    latency = document.get("latency", [])
    rtt = _by_name(latency, "slot_round_trip_64B")
    push = _by_name(latency, "property_push_propagation")
    signal = _by_name(latency, "signal_propagation")
    big = _by_name(latency, "slot_round_trip_4096B")
    if not rtt:
        checks.append(Check("transport.metrics", False, "no slot_round_trip_64B measurement"))
        return

    for label, one_way in (("push", push), ("signal", signal)):
        if not one_way:
            continue
        checks.append(
            Check(
                f"transport.one_way_cheaper_than_round_trip[{label}]",
                one_way["p50"] < rtt["p50"],
                f"{label} p50 {one_way['p50']:.4g} ms vs round trip {rtt['p50']:.4g} ms",
            )
        )

    if big:
        # "RTT roughly flat from 64 B to 4 KB": QtRO framing, not payload, dominates.
        # The baseline sits at 1.15x, so 2x is the band that leaves a shared runner room.
        grew = _ratio(big["p50"], rtt["p50"])
        checks.append(
            Check(
                "transport.payload_size_barely_matters",
                grew < 2.0,
                f"4 KB round trip is {grew:.2f}x the 64 B one (band: < 2x)",
            )
        )

    throughput = _by_name(document.get("throughput", []), "slot_throughput_64B")
    if throughput:
        # Calls pipeline, so throughput must beat the serialized 1/RTT ceiling. If it
        # ever drops to it, something has started waiting for each reply.
        serialized = 1000.0 / rtt["p50"]
        checks.append(
            Check(
                "transport.calls_pipeline",
                throughput["value"] > serialized,
                f"{throughput['value']:.3g} calls/s against a serialized ceiling of "
                f"{serialized:.3g} calls/s",
            )
        )

    rows = document.get("model_replication", [])
    monotone = all(a["ms"] <= b["ms"] for a, b in zip(rows, rows[1:]))
    if rows:
        checks.append(
            Check(
                "transport.model_replication_grows_with_rows",
                monotone,
                ", ".join(f"{r['rows']} rows {r['ms']:.3g} ms" for r in rows),
            )
        )


def _check_mesh(document: Mapping[str, Any], checks: List[Check]) -> None:
    latency = document.get("latency", [])
    mtls_setup = _by_name(latency, "mtls_loopback.connection_setup")
    local_setup = _by_name(latency, "local_socket.connection_setup")
    mtls_rtt = _by_name(latency, "mtls_loopback.slot_round_trip_64B")
    local_rtt = _by_name(latency, "local_socket.slot_round_trip_64B")

    if mtls_setup and local_setup:
        # The whole justification for keeping `transport: local` as an explicit opt-in.
        # The benchmarking plan says measure it rather than assume it; the baseline is
        # ~113x, so the gate only asks that the ordering survives.
        factor = _ratio(mtls_setup["p50"], local_setup["p50"])
        checks.append(
            Check(
                "mesh.local_setup_is_cheaper_than_mtls",
                factor > ASSERT_MARGIN,
                f"mutual-TLS setup is {factor:.1f}x the local socket "
                f"({mtls_setup['p50']:.3g} ms vs {local_setup['p50']:.3g} ms)",
            )
        )
    else:
        checks.append(Check("mesh.modes", False, "connection_setup missing for one of the modes"))

    if mtls_rtt and local_rtt:
        # "Once a link is up, mutual TLS on loopback is cheap." Baseline 1.75x; a
        # regression here would mean per-message crypto cost had ballooned.
        overhead = _ratio(mtls_rtt["p50"], local_rtt["p50"])
        checks.append(
            Check(
                "mesh.steady_state_mtls_overhead_is_small",
                overhead < 5.0,
                f"mutual TLS costs {overhead:.2f}x the local socket per round trip "
                f"(band: < 5x)",
            )
        )

    mtls_tp = _by_name(document.get("throughput", []), "mtls_loopback.slot_throughput_64B")
    local_tp = _by_name(document.get("throughput", []), "local_socket.slot_throughput_64B")
    if mtls_tp and local_tp:
        # Reported, never enforced: 1.2x in the baseline is inside a shared runner's noise.
        checks.append(
            Check(
                "mesh.throughput_ordering",
                local_tp["value"] >= mtls_tp["value"],
                f"local {local_tp['value']:.3g} vs mutual TLS {mtls_tp['value']:.3g} calls/s",
                enforced=False,
            )
        )


def _check_sessions(document: Mapping[str, Any], checks: List[Check]) -> None:
    sweep = sorted(document.get("sweep", []), key=lambda row: row["sessions"])
    if len(sweep) < 2:
        checks.append(Check("sessions.sweep", False, "need at least two sizes to judge growth"))
        return
    smallest, largest = sweep[0], sweep[-1]
    span = _ratio(largest["sessions"], smallest["sessions"])

    for name, label in (("lookup_hit_ns", "lookup"), ("hasscope_set_ns", "hasScope")):
        growth = _ratio(largest[name], smallest[name])
        checks.append(
            Check(
                f"sessions.{label}_stays_constant_time",
                growth < 10.0,
                f"{label} grew {growth:.2f}x while the table grew {span:.0f}x "
                f"({smallest[name]:.0f} -> {largest[name]:.0f} ns; band: < 10x, cache not "
                f"algorithm)",
            )
        )

    # The one that matters most. createSession() used to run a full-table purge, making
    # it O(live sessions) -- 306 us at 100k. The expiry queue made it amortized O(1) at
    # ~600 ns. Reintroducing the walk would show up here as a 500x spread, so a 5x band
    # catches it with room to spare and no chance of flapping.
    creates = [row["create_ns"] for row in sweep]
    spread = _ratio(max(creates), min(creates))
    checks.append(
        Check(
            "sessions.create_is_amortized_constant_time",
            spread < 5.0,
            f"createSession spread {spread:.2f}x across a {span:.0f}x table "
            f"({min(creates):.0f} -> {max(creates):.0f} ns; band: < 5x)",
        )
    )

    snapshots = [(row["sessions"], row["snapshot_ms"]) for row in sweep]
    checks.append(
        Check(
            "sessions.snapshot_is_linear_by_design",
            True,
            ", ".join(f"{n} -> {ms:.3g} ms" for n, ms in snapshots)
            + " (off the request path: once per consumer connect)",
            enforced=False,
        )
    )


def _check_fanout(document: Mapping[str, Any], checks: List[Check]) -> None:
    sweep = document.get("sweep", [])
    interest_k = document.get("interest_k")
    if not sweep or not interest_k:
        checks.append(Check("fanout.sweep", False, "no sweep, or no interest_k recorded"))
        return

    # Interest management's entire promise: each player's slice stops growing with the
    # world. Exact arithmetic, so it holds on any machine.
    offenders = [
        f"n={row['consumers']} carried {row['rows_per_session']} rows"
        for row in sweep
        if row["mode"] == "per_session_interest"
        and row["rows_per_session"] != min(row["consumers"], interest_k)
    ]
    checks.append(
        Check(
            "fanout.interest_management_holds_the_slice_flat",
            not offenders,
            f"every per-session slice is min(N, k={interest_k}) rows"
            if not offenders
            else "; ".join(offenders),
        )
    )

    by_size: Dict[int, Dict[str, Mapping[str, Any]]] = {}
    for row in sweep:
        by_size.setdefault(row["consumers"], {})[row["mode"]] = row

    for size in sorted(by_size):
        modes = by_size[size]
        naive = modes.get("per_session_naive")
        interest = modes.get("per_session_interest")
        if not naive or not interest:
            continue
        if naive["rows_per_session"] <= interest["rows_per_session"]:
            continue  # below k there is nothing to filter, so there is nothing to claim
        saved = _ratio(naive["publish_cpu"]["p50"], interest["publish_cpu"]["p50"])
        checks.append(
            Check(
                f"fanout.interest_is_cheaper_to_publish[n={size}]",
                saved > 1.0,
                f"naive publish {naive['publish_cpu']['p50']:.3g} ms vs interest "
                f"{interest['publish_cpu']['p50']:.3g} ms ({saved:.2f}x)",
            )
        )

    naive_curve = [
        (row["consumers"], row["publish_cpu"]["p50"])
        for row in sorted(sweep, key=lambda r: r["consumers"])
        if row["mode"] == "per_session_naive"
    ]
    if len(naive_curve) >= 2:
        checks.append(
            Check(
                "fanout.naive_publish_growth",
                True,
                " -> ".join(f"n={n}: {ms:.3g} ms" for n, ms in naive_curve)
                + " (quadratic by construction; this is the shape the tutorial warns about)",
                enforced=False,
            )
        )


def _check_persistence(document: Mapping[str, Any], checks: List[Check]) -> None:
    scalars = document.get("scalars", [])
    autocommit = _by_name(scalars, "sqlite_write_autocommit_rate")
    batched = _by_name(scalars, "sqlite_write_batched_rate")
    if autocommit and batched:
        factor = _ratio(batched["value"], autocommit["value"])
        checks.append(
            Check(
                "persistence.batching_beats_autocommit",
                factor > ASSERT_MARGIN,
                f"one transaction is {factor:.2f}x autocommit "
                f"({batched['value']:.4g} vs {autocommit['value']:.4g} rows/s)",
            )
        )

    latency = document.get("latency", [])
    contended = _by_name(latency, "sqlite_write_contended")
    plain = _by_name(latency, "sqlite_write_autocommit")
    if contended:
        # The safety claim: with a second connection hammering the same WAL file, the
        # busy-timeout retry always wins the lock in the end and no write is refused. The
        # harness counts the refusals rather than inferring them from a wall clock, so this
        # is a count and not a duration, and it means the same thing on any machine. It used
        # to compare the worst single write against the 5 s timeout, which read like a safety
        # bound but was really a claim about one outlier on a quiet workstation: a shared CI
        # runner that descheduled the writer once produced 3.9 s while every other write was
        # sub-millisecond and none of them failed.
        abandoned = _by_name(scalars, "sqlite_contended_writes_abandoned")
        attempted = _by_name(scalars, "sqlite_contended_writes_attempted")
        if abandoned is None:
            checks.append(
                Check(
                    "persistence.contended_writer_never_gives_up",
                    False,
                    "this baseline predates sqlite_contended_writes_abandoned; re-run "
                    "benchmarks/persistence/run-bench.sh to record it",
                )
            )
        else:
            total = attempted["value"] if attempted else 0
            checks.append(
                Check(
                    "persistence.contended_writer_never_gives_up",
                    abandoned["value"] == 0,
                    f"{abandoned['value']:.0f} of {total:.0f} contended writes were refused "
                    f"by the 5 s busy timeout",
                )
            )
        if plain:
            inflation = _ratio(contended["p50"], plain["p50"])
            checks.append(
                Check(
                    "persistence.contention_does_not_move_the_median",
                    inflation < 3.0,
                    f"median write under contention is {inflation:.2f}x the quiet one "
                    f"(band: < 3x)",
                )
            )
            # The tail, as a ratio rather than a wall clock. A writer that starts losing to
            # the competitor systematically shows up here; one descheduled sample does not,
            # which is the difference between this and the max it replaced.
            tail = _ratio(contended["p99"], plain["p99"])
            checks.append(
                Check(
                    "persistence.contention_does_not_move_the_tail",
                    tail < 5.0,
                    f"p99 write under contention is {tail:.2f}x the quiet one (band: < 5x)",
                )
            )
        checks.append(
            Check(
                "persistence.contended_worst_case",
                True,
                f"worst contended write {contended['max']:.3g} ms against a 5000 ms busy "
                f"timeout (one sample, and it moves with the scheduler; the enforced claim "
                f"is that none was refused)",
                enforced=False,
            )
        )

    hit = _by_name(scalars, "cache_get_hit")
    evicting = _by_name(scalars, "cache_set_under_eviction")
    if hit and evicting:
        checks.append(
            Check(
                "persistence.eviction_costs_more_than_a_hit",
                True,
                f"cache set under eviction {evicting['value']:.4g} ns/op against a "
                f"{hit['value']:.3g} ns/op hit (O(bound) recency list; a note for very "
                f"large bounds)",
                enforced=False,
            )
        )


def _check_capstone(document: Mapping[str, Any], checks: List[Check]) -> None:
    sweep = sorted(document.get("sweep", []), key=lambda row: row["players"])
    interest_k = document.get("interest_k")
    hz = document.get("hz")
    if not sweep or not interest_k or not hz:
        checks.append(Check("capstone.sweep", False, "no sweep, or no interest_k/hz recorded"))
        return

    offenders = [
        f"{row['players']} players carried {row['rows_per_session']} rows"
        for row in sweep
        if row["rows_per_session"] > interest_k
    ]
    checks.append(
        Check(
            "capstone.payload_never_exceeds_the_interest_cap",
            not offenders,
            f"every player's slice is at most k={interest_k} rows"
            if not offenders
            else "; ".join(offenders),
        )
    )

    # A player cannot be handed more snapshots than the loop publishes. This is not a
    # performance claim, it is an arithmetic one, and it is enforced because the only way
    # to break it is for the harness to be measuring the wrong thing. It has happened: the
    # rate was once derived by subtracting a coalescing counter, so a backed-up link
    # draining the previous window's backlog reported 84.9/s from a 30 Hz tick, and the
    # sweep's saturated end looked healthier than its healthy end. Nothing here caught it.
    # A small tolerance covers the window boundary, not a factor of three.
    overrun = [
        f"{row['players']} players received {row['snapshot_rate_hz']:.1f}/s"
        for row in sweep
        if row.get("snapshot_rate_hz", 0.0) > hz * 1.05
    ]
    checks.append(
        Check(
            "capstone.snapshot_rate_never_exceeds_the_tick_rate",
            not overrun,
            f"no player is handed more than the {hz} Hz the loop publishes"
            if not overrun
            else "; ".join(overrun) + f" against a {hz} Hz loop",
        )
    )

    # The point of the capstone is to find the ceiling, so the ceiling is reported, not
    # gated: past it the fixed-rate loop stops holding its cadence, and that is the
    # honest limit of a single-edge deployment rather than a defect.
    period_ms = 1000.0 / hz
    saturated = next(
        (row["players"] for row in sweep if row["tick_jitter"]["p50"] > period_ms / 2), None
    )
    checks.append(
        Check(
            "capstone.single_edge_ceiling",
            True,
            f"tick cadence holds to {sweep[-1]['players']} players"
            if saturated is None
            else f"cadence holds below {saturated} players and degrades from there "
            f"(period {period_ms:.0f} ms)",
            enforced=False,
        )
    )


def _check_edge_http(document: Mapping[str, Any], checks: List[Check]) -> None:
    rows = document.get("results", [])
    if not rows:
        checks.append(Check("edge-http.results", False, "no measured routes"))
        return
    failed = [
        f"{row['test']}@{row['connections']}c: {row['errors']}"
        for row in rows
        if row.get("errors")
    ]
    checks.append(
        Check(
            "edge-http.no_errors_under_load",
            not failed,
            f"{len(rows)} route/connection combinations served with zero errors"
            if not failed
            else "; ".join(failed),
        )
    )
    idle = [
        f"{row['test']}@{row['connections']}c"
        for row in rows
        if row.get("requests_per_sec", 0) <= 0
    ]
    checks.append(
        Check(
            "edge-http.every_route_served_traffic",
            not idle,
            "every route returned a positive request rate" if not idle else "; ".join(idle),
        )
    )


def _check_client_bundle(document: Mapping[str, Any], checks: List[Check]) -> None:
    files = document.get("files", [])
    if not files:
        checks.append(Check("client-bundle.files", False, "no files weighed"))
        return
    sums = {
        f"total_{name}": sum(int(entry[name]) for entry in files)
        for name in ("raw", "gzip", "brotli")
    }
    wrong = [
        f"{key}: {document.get(key)} recorded, {value} summed"
        for key, value in sums.items()
        if document.get(key) != value
    ]
    checks.append(
        Check(
            "client-bundle.totals_match_the_files",
            not wrong,
            f"{len(files)} files sum to the recorded totals" if not wrong else "; ".join(wrong),
        )
    )
    inverted = [
        entry["name"]
        for entry in files
        if not (entry["brotli"] <= entry["gzip"] <= entry["raw"])
    ]
    checks.append(
        Check(
            "client-bundle.compression_helps_every_asset",
            not inverted,
            "brotli <= gzip <= raw for every asset"
            if not inverted
            else f"inverted for: {', '.join(inverted)}",
        )
    )


def _check_client_frame_time(document: Mapping[str, Any], checks: List[Check]) -> None:
    buckets = document.get("buckets", [])
    if not buckets:
        checks.append(Check("client-frame-time.buckets", False, "no frame samples bucketed"))
        return
    blobs = [row["blobs"] for row in buckets]
    checks.append(
        Check(
            "client-frame-time.buckets_ascend",
            blobs == sorted(blobs),
            f"{len(buckets)} buckets from {blobs[0]} to {blobs[-1]} blobs",
        )
    )
    cold = document.get("cold_start_ms", 0)
    checks.append(
        Check(
            "client-frame-time.cold_start_recorded",
            _finite(cold) and cold > 0,
            f"navigation to first frame {cold} ms",
        )
    )
    if "multi" in str(document.get("label", "")):
        # The threaded kit is only threaded when the page is cross-origin isolated; a
        # false here means the numbers are single-threaded ones wearing the wrong label.
        checks.append(
            Check(
                "client-frame-time.threaded_kit_was_cross_origin_isolated",
                bool(document.get("cross_origin_isolated")),
                "the page reported crossOriginIsolated, so SharedArrayBuffer was available",
            )
        )


def _check_remote_pages(document: Mapping[str, Any], checks: List[Check]) -> None:
    remote = document.get("remote", {})
    compiled_in = document.get("compiled_in", {})
    saving = document.get("saving", {})
    wrong: List[str] = []
    for measure, recorded in (
        ("raw", "raw_bytes"),
        ("gzip", "gzip_bytes"),
        ("brotli", "brotli_bytes"),
    ):
        expected = compiled_in[f"total_{measure}"] - remote[f"total_{measure}"]
        if saving.get(recorded) != expected:
            wrong.append(f"{recorded}: {saving.get(recorded)} recorded, {expected} computed")
    checks.append(
        Check(
            "remote-pages.saving_is_the_difference_it_claims",
            not wrong,
            f"saving = compiled-in minus remote on all three measures "
            f"({saving.get('brotli_bytes')} Brotli bytes)"
            if not wrong
            else "; ".join(wrong),
        )
    )
    checks.append(
        Check(
            "remote-pages.edge_delivery_removes_weight",
            remote["total_brotli"] < compiled_in["total_brotli"],
            f"remote {remote['total_brotli']} vs compiled-in {compiled_in['total_brotli']} "
            f"Brotli bytes",
        )
    )


def _check_buildtime(document: Mapping[str, Any], checks: List[Check]) -> None:
    sweep = document.get("sweep", [])
    if not sweep:
        checks.append(Check("buildtime.sweep", False, "no entities were built"))
        return

    for row in sweep:
        # The claim: building nothing costs a fraction of building everything. It is the
        # only thing that tells a rebuilding-everything build system from an incremental
        # one, and nothing else in the repository can: a build that recompiles the world
        # on every invocation still passes every correctness test there is.
        share = _ratio(row["noop_s"], row["clean_s"])
        checks.append(
            Check(
                f"buildtime.the_build_is_incremental[{row['target']}]",
                share < 1.0 / ASSERT_MARGIN,
                f"a no-op build costs {share * 100:.0f}% of a clean one "
                f"({row['noop_s']:.2f}s of {row['clean_s']:.2f}s; band: < 50%)",
            )
        )
        # The sharper form of the same claim, and the one that actually guards the defect.
        # A no-op that costs a third of a clean build passes the band above while doing a
        # full recompile, which is exactly what an unconditionally rewritten `main.cpp`
        # produces: regeneration moves every modification time, and the compiler reads
        # modification times. Content-addressed generation (synqt.writer) puts a no-op at
        # well under 1%, so the band that notices a regression is this one.
        checks.append(
            Check(
                f"buildtime.a_no_op_build_compiles_nothing[{row['target']}]",
                share < 0.05,
                f"a no-op build costs {share * 100:.1f}% of a clean one "
                f"({row['noop_s']:.2f}s of {row['clean_s']:.2f}s; band: < 5%)",
            )
        )
        if "touched_s" in row:
            edit = _ratio(row["touched_s"], row["clean_s"])
            # A WebAssembly client is link-dominated, and no edit can avoid the link. Measured
            # on the gavel client: touching Main.qml costs 16.9 s to compile the one translation
            # unit qmlcachegen produced from it and 36.3 s in the Emscripten link that follows,
            # so the link alone is over half of a clean build. Held to the service band, that row
            # fails for being a WebAssembly target rather than for being unincremental, which
            # would teach the reader to ignore the check. The band that still means something
            # here is that an edit costs less than a clean build, plus the no-op check above,
            # which the client passes at 0.2%.
            band = 0.90 if row["kind"] == "client" else 1.0 / ASSERT_MARGIN
            checks.append(
                Check(
                    f"buildtime.one_edit_does_not_rebuild_everything[{row['target']}]",
                    edit < band,
                    f"rebuilding after touching {row.get('touched_file', 'one file')} costs "
                    f"{edit * 100:.0f}% of a clean build ({row['touched_s']:.2f}s; "
                    f"band: < {band * 100:.0f}%)",
                )
            )

    generation = _by_name(document.get("latency", []), "contract_generation")
    slowest_clean = max(row["clean_s"] for row in sweep)
    if generation and slowest_clean:
        share = _ratio(generation["p50"] / 1000.0, slowest_clean)
        checks.append(
            Check(
                "buildtime.codegen_is_a_rounding_error",
                share < 0.10,
                f"lowering {generation.get('contracts', '?')} contracts takes "
                f"{generation['p50']:.0f} ms, {share * 100:.1f}% of a clean build "
                f"(band: < 10%)",
            )
        )


INVARIANTS: Dict[str, Callable[[Mapping[str, Any], List[Check]], None]] = {
    "transport": _check_transport,
    "mesh": _check_mesh,
    "sessions": _check_sessions,
    "fanout": _check_fanout,
    "persistence": _check_persistence,
    "capstone": _check_capstone,
    "edge-http": _check_edge_http,
    "client-bundle": _check_client_bundle,
    "client-frame-time": _check_client_frame_time,
    "remote-pages": _check_remote_pages,
    "buildtime": _check_buildtime,
}


def check_document(document: Mapping[str, Any], path: Optional[Path] = None) -> Report:
    """Validate one result: well-formed first, then the claims it is supposed to support."""
    kind = kind_of(document, path)
    report = Report(path=path or Path("<memory>"), kind=kind)
    _check_metadata(document, kind, report.checks)
    _check_distributions(document, report.checks)
    invariant = INVARIANTS.get(kind)
    if invariant is None:
        report.checks.append(
            Check("invariants", False, f"no invariants are defined for benchmark `{kind}`")
        )
    else:
        invariant(document, report.checks)
    return report


def check_file(path: Path) -> Report:
    return check_document(load(path), path)


# Comparison: same benchmark, same runner, two points in time


@dataclass
class Delta:
    key: str
    baseline: float
    candidate: float
    unit: str
    lower_is_better: bool
    noisy: bool

    @property
    def change(self) -> float:
        """Signed fractional change, positive when the candidate got worse."""
        if self.baseline == 0:
            return math.inf if self.candidate else 0.0
        raw = (self.candidate - self.baseline) / abs(self.baseline)
        return raw if self.lower_is_better else -raw

    def regressed(self, tolerance: float) -> bool:
        return self.change > tolerance


@dataclass
class Comparison:
    deltas: List[Delta]
    missing: List[str]
    added: List[str]
    tolerance: float
    strict: bool

    @property
    def regressions(self) -> List[Delta]:
        return [
            delta
            for delta in self.deltas
            if (self.strict or not delta.noisy) and delta.regressed(self.tolerance)
        ]

    @property
    def ok(self) -> bool:
        return not self.regressions


def compare(
    baseline: Mapping[str, Any],
    candidate: Mapping[str, Any],
    tolerance: float = 0.25,
    strict: bool = False,
) -> Comparison:
    """Diff two runs of the same benchmark, metric by metric.

    Only meaningful when both came off the same machine. `tolerance` is the fraction a
    metric may move in the worse direction before it counts; p95/p99/max are diffed and
    printed but do not fail unless `strict`, because tail latency on any real machine
    moves for reasons that are not the code.
    """
    base_metrics = flatten(baseline)
    cand_metrics = flatten(candidate)
    shared = [key for key in base_metrics if key in cand_metrics]
    deltas = [
        Delta(
            key=key,
            baseline=base_metrics[key].value,
            candidate=cand_metrics[key].value,
            unit=base_metrics[key].unit,
            lower_is_better=base_metrics[key].lower_is_better,
            noisy=base_metrics[key].noisy,
        )
        for key in shared
    ]
    return Comparison(
        deltas=deltas,
        missing=[key for key in base_metrics if key not in cand_metrics],
        added=[key for key in cand_metrics if key not in base_metrics],
        tolerance=tolerance,
        strict=strict,
    )


# Command line


def _print_report(report: Report, verbose: bool) -> None:
    name = report.path.name if report.path else report.kind
    verdict = "PASS" if report.ok else "FAIL"
    print(f"{verdict}  {name}  [{report.kind}]")
    for check in report.checks:
        if check.ok and not check.enforced and not verbose:
            print(f"      note  {check.name}: {check.detail}")
        elif check.ok and not verbose:
            continue
        else:
            print(f"      {check.status:>4}  {check.name}: {check.detail}")


def _command_check(args: argparse.Namespace) -> int:
    paths = [Path(p) for p in args.files] if args.files else committed_baselines()
    if not paths:
        print("no baselines found", file=sys.stderr)
        return 1
    failures = 0
    for path in paths:
        try:
            report = check_file(path)
        except BaselineError as error:
            print(f"FAIL  {path}\n      {error}")
            failures += 1
            continue
        _print_report(report, args.verbose)
        failures += 0 if report.ok else 1
    print()
    print(f"{len(paths) - failures}/{len(paths)} baselines pass their invariants")
    return 1 if failures else 0


def _command_compare(args: argparse.Namespace) -> int:
    baseline = load(Path(args.baseline))
    candidate = load(Path(args.candidate))
    base_kind = kind_of(baseline, Path(args.baseline))
    cand_kind = kind_of(candidate, Path(args.candidate))
    if base_kind != cand_kind:
        print(f"refusing to compare `{base_kind}` against `{cand_kind}`", file=sys.stderr)
        return 1
    base_host = baseline.get("host")
    cand_host = candidate.get("host")
    if base_host and cand_host and base_host != cand_host:
        print(
            f"warning: `{base_host}` against `{cand_host}`. Absolute benchmark numbers "
            f"only compare on one machine; read this as indicative.",
            file=sys.stderr,
        )

    result = compare(baseline, candidate, args.tolerance, args.strict)
    if not result.deltas:
        # Almost always a sweep run with different sizes. Reporting "nothing regressed"
        # for a comparison that compared nothing is the one answer that must not happen.
        print(
            "no metric appears in both runs, so there is nothing to compare. Re-run the "
            "candidate with the baseline's parameters.",
            file=sys.stderr,
        )
        return 1

    width = max((len(d.key) for d in result.deltas), default=10)
    for delta in sorted(result.deltas, key=lambda d: -d.change):
        marker = "  REGRESSED" if delta in result.regressions else ""
        tail = " (tail, not gated)" if delta.noisy and not args.strict else ""
        print(
            f"  {delta.key:<{width}}  {delta.baseline:>12.6g} -> {delta.candidate:>12.6g} "
            f"{delta.unit:<7} {delta.change * 100:+7.1f}%{marker}{tail}"
        )
    if result.missing:
        print(f"\n  {len(result.missing)} metric(s) the candidate did not report: "
              f"{', '.join(result.missing[:8])}")
    if result.added:
        print(f"  {len(result.added)} new metric(s): {', '.join(result.added[:8])}")
    print()
    if result.ok:
        print(f"no metric regressed by more than {args.tolerance * 100:.0f}%")
        return 0
    print(f"{len(result.regressions)} metric(s) regressed by more than "
          f"{args.tolerance * 100:.0f}%")
    return 1


def _command_show(args: argparse.Namespace) -> int:
    document = load(Path(args.file))
    metrics = flatten(document)
    width = max((len(key) for key in metrics), default=10)
    for key in sorted(metrics):
        metric = metrics[key]
        print(f"  {key:<{width}}  {metric.value:>14.6g} {metric.unit}")
    print(f"\n{len(metrics)} metrics in {kind_of(document, Path(args.file))}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="baselines.py", description="Validate and diff SynQt benchmark baselines."
    )
    subcommands = parser.add_subparsers(dest="command", required=True)

    check = subcommands.add_parser(
        "check", help="validate result files against the claims they support"
    )
    check.add_argument("files", nargs="*", help="result files (default: every committed baseline)")
    check.add_argument("-v", "--verbose", action="store_true", help="print passing checks too")
    check.set_defaults(func=_command_check)

    diff = subcommands.add_parser("compare", help="diff two runs of one benchmark")
    diff.add_argument("baseline")
    diff.add_argument("candidate")
    diff.add_argument(
        "--tolerance",
        type=float,
        default=0.25,
        help="fraction a metric may worsen before it counts (default: 0.25)",
    )
    diff.add_argument(
        "--strict", action="store_true", help="fail on p95/p99/max too, not only on p50 and rates"
    )
    diff.set_defaults(func=_command_compare)

    show = subcommands.add_parser("show", help="print one result flattened to metrics")
    show.add_argument("file")
    show.set_defaults(func=_command_show)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except BaselineError as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
