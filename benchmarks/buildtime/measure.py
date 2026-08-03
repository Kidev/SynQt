# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Time the build: contract generation, and clean and incremental builds per entity.

* **clean**: an empty build directory to a linked artifact.
* **no-op**: `synqt build` again with nothing changed. Should be nearly free.
* **touched**: one QML file edited (a comment line appended, then reverted), then built
  again. A real edit, because generated copies are rewritten only on a content change.

Contract generation is timed separately as a subprocess, interpreter startup included, as
the build runs it.

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
DEFAULT_QT_HOST = "/opt/Qt/6.12.0/gcc_64"

# Put the CLI on the import path for `synqt.appmodel`; `build_env()` does it for the
# subprocesses.
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


def kit_version(qt_host: str) -> str:
    """The Qt version of the kit the builds ran on, read from the kit, never from the pin."""
    config = Path(qt_host) / "lib" / "cmake" / "Qt6" / "Qt6ConfigVersionImpl.cmake"
    try:
        for line in config.read_text(encoding="utf-8").splitlines():
            if line.startswith("set(PACKAGE_VERSION "):
                return line.split('"')[1]
    except (OSError, IndexError):
        pass
    raise SystemExit(f"cannot tell which Qt the kit at {qt_host} is (no {config})")


def build_env(qt_host: str) -> Dict[str, str]:
    env = dict(os.environ)
    env["QT_HOST"] = qt_host
    # The compiler cache is off for the clean build, which would otherwise measure the
    # cache.
    env["CCACHE_DISABLE"] = "1"
    tools = f"{REPO_ROOT / 'tools' / 'synqt'}{os.pathsep}{REPO_ROOT / 'tools' / 'synqtc'}"
    existing = env.get("PYTHONPATH")
    env["PYTHONPATH"] = f"{tools}{os.pathsep}{existing}" if existing else tools
    return env


def written_contracts(project: Path, env: Dict[str, str]) -> List[Path]:
    """Write the project contracts under ``generated/`` as the build does, and return the files."""
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
    """Append a comment line to `qml`, time the rebuild, and restore the file byte for byte. A
    timestamp alone would not change the generated copy.
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
    # Record the configuration that was built.
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
        "qt_version": kit_version(args.qt_host),
        "cpus": os.cpu_count(),
        "compiler_cache": "bypassed",
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
