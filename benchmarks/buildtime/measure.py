# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Time the build itself. Contract generation, and clean and incremental builds per entity.

This is the one part of the benchmarking plan that is not a measurement harness. There is
nothing to instrument. The build steps already exist and this times around them. What it
reports, per entity:

* **clean**: an empty build directory to a linked artifact. The number a new contributor
  or a cold CI runner waits for.
* **no-op**: `synqt build` again with nothing changed. This should be nearly free, and
  it is the number that says whether the build is incremental at all.
* **touched**: one QML file edited (a comment line appended, then reverted), then build
  again. The edit-rebuild cycle, and what `synqt dev` pays on every hot reload. It has to
  be a real edit. The generated copy of an entity's QML is written only when the content
  differs, so a timestamp alone changes nothing downstream.

The no-op is the reason this exists. A build system that quietly
rebuilds everything when nothing changed still passes every correctness test in the
repository, and the only thing that catches it is a clock. Codegen runs at CMake configure
time (`cmake/SynQtContracts.cmake`), so anything that rewrites a generated header on every
configure invalidates every translation unit that includes it, and the cost lands on every
edit of every day.

Contract generation is timed separately, as a subprocess, because that is how the build
invokes it. Interpreter startup included, since the build pays that too.

    python benchmarks/buildtime/measure.py --project examples/gavel --out results/...json
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_QT_HOST = "/opt/Qt/6.11.1/gcc_64"

# This module reads the topology through `synqt.appmodel`, so it needs the CLI on the path
# whether or not the caller put it there. `build_env()` below sets it for the subprocesses;
# this is for the import in this process.
sys.path.insert(0, str(REPO_ROOT / "tools" / "synqt"))


def host_label() -> str:
    """The operating system, the way the C++ harnesses report it (QSysInfo pretty name)."""
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            if line.startswith("PRETTY_NAME="):
                return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return platform.system()


def distribution(name: str, samples: List[float], unit: str = "ms") -> Dict[str, Any]:
    """The same p50/p95/p99 block every other harness writes, so one reader serves all."""
    ordered = sorted(samples)

    def percentile(fraction: float) -> float:
        if not ordered:
            return 0.0
        index = min(int(fraction * (len(ordered) - 1) + 0.5), len(ordered) - 1)
        return ordered[index]

    return {
        "name": name,
        "unit": unit,
        "samples": len(ordered),
        "min": ordered[0] if ordered else 0.0,
        "p50": percentile(0.50),
        "p95": percentile(0.95),
        "p99": percentile(0.99),
        "mean": statistics.fmean(ordered) if ordered else 0.0,
        "max": ordered[-1] if ordered else 0.0,
    }


def run(command: List[str], cwd: Path, env: Dict[str, str]) -> float:
    """Run one build step and return its wall-clock seconds, failing loudly if it fails."""
    started = time.perf_counter()
    completed = subprocess.run(
        command, cwd=str(cwd), env=env, capture_output=True, text=True, check=False
    )
    elapsed = time.perf_counter() - started
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout[-4000:])
        sys.stderr.write(completed.stderr[-4000:])
        raise SystemExit(f"build step failed ({' '.join(command)})")
    return elapsed


def build_env(qt_host: str) -> Dict[str, str]:
    env = dict(os.environ)
    env["QT_HOST"] = qt_host
    tools = f"{REPO_ROOT / 'tools' / 'synqt'}{os.pathsep}{REPO_ROOT / 'tools' / 'synqtc'}"
    existing = env.get("PYTHONPATH")
    env["PYTHONPATH"] = f"{tools}{os.pathsep}{existing}" if existing else tools
    return env


def written_contracts(project: Path, env: Dict[str, str]) -> List[Path]:
    """Write the project's contracts the way the build does, and return the files.

    A contract is not a file the author keeps: the connect point's ``export:`` block in
    ``synqt.yaml`` is the contract, and the build writes it out under ``generated/``
    before the compiler ever sees it. So the files are produced here first, into the
    project itself, exactly where a build puts them.
    """
    script = (
        "import json, sys, yaml\n"
        "from synqt import contractgen\n"
        "project = sys.argv[1]\n"
        "config = yaml.safe_load(open(sys.argv[2], encoding='utf-8'))\n"
        "print(json.dumps(contractgen.write_contracts(project, config)))\n"
    )
    completed = subprocess.run(
        [sys.executable, "-c", script, str(project), str(project / "synqt.yaml")],
        cwd=str(REPO_ROOT), env=env, capture_output=True, text=True, check=False,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr[-4000:])
        raise SystemExit(f"could not write the contracts of {project}")
    return [project / relative for relative in json.loads(completed.stdout)]


def time_contract_generation(project: Path, env: Dict[str, str], repeats: int) -> Dict[str, Any]:
    """Time `synqtc` over the project's contracts, as a subprocess, as the build runs it."""
    contracts = sorted(written_contracts(project, env))
    if not contracts:
        raise SystemExit(f"no connect point in {project / 'synqt.yaml'} declares an export")
    out_dir = REPO_ROOT / "build" / "bench-buildtime" / "codegen"
    samples: List[float] = []
    for _ in range(repeats):
        shutil.rmtree(out_dir, ignore_errors=True)
        out_dir.mkdir(parents=True, exist_ok=True)
        started = time.perf_counter()
        for contract in contracts:
            subprocess.run(
                [sys.executable, "-m", "synqtc", str(contract), "--out", str(out_dir), "--quiet"],
                cwd=str(REPO_ROOT / "tools" / "synqtc"),
                env=env,
                capture_output=True,
                check=True,
            )
        samples.append((time.perf_counter() - started) * 1000.0)
    block = distribution("contract_generation", samples)
    block["contracts"] = len(contracts)
    return block


def entities_of(project: Path, include_client: bool) -> List[Dict[str, str]]:
    """Read the topology rather than guessing: which entities exist, and which are clients."""
    import yaml

    from synqt import appmodel

    config = yaml.safe_load((project / "synqt.yaml").read_text(encoding="utf-8"))
    targets: List[Dict[str, str]] = []
    for entity in appmodel.entities(config):
        if appmodel.is_client(entity) and not include_client:
            continue
        targets.append({
            "target": entity["name"],
            "kind": appmodel.entity_type(entity),
            "dir": appmodel.entity_dir(entity),
        })
    return targets


def first_qml(project: Path, entity: Dict[str, str]) -> Optional[Path]:
    """The QML file the touched-build measurement edits."""
    files = sorted((project / entity["dir"]).glob("*.qml"))
    return files[0] if files else None


def time_one_edit(qml: Path, command: List[str], env: Dict[str, str]) -> float:
    """Append a comment line to `qml`, time the rebuild, and put the file back.

    It has to be a real edit. `synqt build` copies an entity's QML into ``generated/``
    through `synqt.writer.write_if_changed`, which compares content, so moving a
    timestamp alone leaves the generated copy untouched and correctly rebuilds nothing;
    a `touch()` here measured 0.1 s and would have reported it as the edit-rebuild cycle.
    The file is restored byte for byte afterwards, and the generated copy follows on the
    next build.
    """
    original = qml.read_bytes()
    try:
        qml.write_bytes(original.rstrip(b"\n") + b"\n\n// buildtime benchmark: one edit\n")
        return run(command, REPO_ROOT, env)
    finally:
        qml.write_bytes(original)


def measure_entity(
    project: Path, entity: Dict[str, str], env: Dict[str, str], build_flags: List[str]
) -> Dict[str, Any]:
    name = entity["target"]
    command = [
        sys.executable, "-m", "synqt", "build",
        "--project-dir", str(project), "--entity", name, *build_flags,
    ]

    shutil.rmtree(project / "build", ignore_errors=True)
    clean = run(command, REPO_ROOT, env)
    noop = run(command, REPO_ROOT, env)

    touched: Optional[float] = None
    qml = first_qml(project, entity)
    if qml is not None:
        touched = time_one_edit(qml, command, env)

    row: Dict[str, Any] = {
        "target": name,
        "kind": entity["kind"],
        "clean_s": round(clean, 3),
        "noop_s": round(noop, 3),
    }
    if touched is not None:
        row["touched_s"] = round(touched, 3)
        row["touched_file"] = qml.name
    return row


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="measure.py", description="Time contract generation and per-entity builds."
    )
    parser.add_argument("--project", default="examples/gavel", help="project directory to build")
    parser.add_argument("--out", default="", help="write the JSON result here")
    parser.add_argument(
        "--repeats", type=int, default=10, help="contract-generation samples (default: 10)"
    )
    parser.add_argument(
        "--include-client",
        action="store_true",
        help="also build the WebAssembly client (needs the Emscripten kit; several minutes)",
    )
    # `synqt build` defaults to --release (cli.py), so the report says release unless
    # asked otherwise. Recording the wrong configuration would make two baselines look
    # comparable when they measured different builds.
    parser.add_argument("--debug", action="store_true", help="build debug rather than release")
    parser.add_argument(
        "--qt-host", default=os.environ.get("QT_HOST", DEFAULT_QT_HOST), help="Qt kit path"
    )
    args = parser.parse_args(argv)

    project = (REPO_ROOT / args.project).resolve()
    if not (project / "synqt.yaml").is_file():
        raise SystemExit(f"no synqt.yaml under {project}")

    env = build_env(args.qt_host)
    build_flags = ["--debug"] if args.debug else ["--release"]

    print(f"SynQt build-time report ({args.project})")
    print(f"Qt kit {args.qt_host} on {host_label()} ({platform.machine()}), "
          f"{os.cpu_count()} CPUs\n")

    generation = time_contract_generation(project, env, args.repeats)
    print(f"contract_generation  {generation['contracts']} contracts  "
          f"n={generation['samples']}  p50={generation['p50']:.1f} ms  "
          f"p95={generation['p95']:.1f} ms")

    sweep: List[Dict[str, Any]] = []
    print(f"\n{'target':<12} {'type':<12} {'clean':>9} {'no-op':>9} {'touched':>9}")
    for entity in entities_of(project, args.include_client):
        row = measure_entity(project, entity, env, build_flags)
        sweep.append(row)
        print(f"{row['target']:<12} {row['kind']:<12} {row['clean_s']:>8.2f}s "
              f"{row['noop_s']:>8.2f}s {row.get('touched_s', float('nan')):>8.2f}s")

    document = {
        "benchmark": "buildtime",
        "path": "contract-generation-and-per-entity-build",
        "project": args.project,
        "host": host_label(),
        "arch": platform.machine(),
        "qt_version": "6.11.1",
        "cpus": os.cpu_count(),
        "configuration": "debug" if args.debug else "release",
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "latency": [generation],
        "sweep": sweep,
    }

    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"\nwrote baseline {out}")
    else:
        print(json.dumps(document, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
