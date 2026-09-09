# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The example systems the CLI ships, and ``synqt new <name> --example <example>``.

An example is a whole project, copied as is. The copy changes the project name and adds a
`.gitignore` and a `.env.example`. Examples are read from the framework root: the checkout,
or the copy a wheel vendors (`tools/synqt/_build_backend.py`).
"""

from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import Any, Dict, List, Tuple

import yaml

from . import appgen, appmodel, licenses, newproject, presets, yamledit


class ExampleError(Exception):
    """A bad example name or an unusable examples directory, surfaced without a traceback."""


# Machine-specific output or secrets. The vendoring step drops the same names.
_SKIP = {"build", "generated", "CMakeUserPresets.json", ".env", "certs", "__pycache__"}


def root() -> Path:
    """Where the shipped examples live. Unlike `appmodel.framework_root`, a temporary unpack
    directory is accepted, since the files are copied out and not referenced.
    """
    override = os.environ.get("SYNQT_ROOT")
    candidates = [Path(override).expanduser().resolve() / "examples"] if override else []
    candidates.append(Path(__file__).resolve().parents[3] / "examples")
    candidates.append(Path(__file__).resolve().parent / "framework" / "examples")
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    raise ExampleError(
        "this build of synqt carries no examples (no examples/ directory beside its "
        "framework sources). Run it from a SynQt checkout, or set SYNQT_ROOT to point at "
        "one.")


def headline(directory: Path) -> str:
    """One line describing an example: its README title after the colon (`# stall: a storefront
    with edge-delivered campaigns`).
    """
    readme = directory / "README.md"
    if not readme.is_file():
        return ""
    for line in readme.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("# "):
            title = line[2:].strip()
            _, _, rest = title.partition(":")
            return (rest.strip() or title).rstrip(".")
    return ""


def available() -> List[Tuple[str, str]]:
    """Every example this build carries, as `(name, headline)`, alphabetically."""
    return sorted((entry.name, headline(entry))
                  for entry in root().iterdir()
                  if entry.is_dir() and (entry / "synqt.yaml").is_file())


def listing() -> str:
    """The examples as `synqt examples` prints them."""
    found = available()
    if not found:
        raise ExampleError(f"{root()} holds no examples")
    width = max(len(name) for name, _ in found)
    lines = [f"  {name.ljust(width)}  {about}" if about else f"  {name}"
             for name, about in found]
    lines.append("")
    lines.append("Start one with: synqt new <directory> --example <name>")
    return "\n".join(lines)


def secrets(node: Any) -> List[str]:
    """Every environment variable an `env:` value in this config reads, named once each."""
    found: set = set()
    if isinstance(node, dict):
        for value in node.values():
            found.update(secrets(value))
    elif isinstance(node, list):
        for value in node:
            found.update(secrets(value))
    elif isinstance(node, str) and node.startswith("env:"):
        name = node[len("env:"):].strip()
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            found.add(name)
    return sorted(found)


def _env_example(config: Dict[str, Any]) -> str:
    """A `.env.example` naming every secret the copied project reads from its environment."""
    header = "# Entity secrets (env: references), never committed\n"
    return header + "".join(f"{name}=\n" for name in secrets(config))


def _copy(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination,
                    ignore=lambda _, names: {name for name in names if name in _SKIP})


def scaffold(parent_dir: os.PathLike[str] | str, name: str, example: str) -> str:
    """Copy the shipped example `example` into a new project directory called `name`."""
    directory = root() / example
    if example != Path(example).name or not (directory / "synqt.yaml").is_file():
        known = ", ".join(found for found, _ in available())
        raise ExampleError(
            f"there is no example called '{example}'. This build ships: {known}")

    destination = Path(parent_dir) / name
    if not appmodel.is_valid_project_name(destination.resolve().name):
        raise newproject.NewProjectError(
            f"'{destination.resolve().name}' cannot name a project: a project name starts "
            "with a letter and is made of letters, digits, underscores and hyphens")
    if destination.exists() and any(destination.iterdir()):
        raise newproject.NewProjectError(f"{destination} already exists and is not empty")
    if destination.exists():
        # Accept an existing empty directory as the target (copytree refuses existing ones).
        destination.rmdir()
    _copy(directory, destination)
    project_name = destination.resolve().name  # `name` may be a path. See newproject

    config_path = destination / "synqt.yaml"
    # Edited in place, keeping the example's comments.
    config_path.write_text(
        yamledit.set_scalar(config_path.read_text(encoding="utf-8"), "project.name",
                            project_name),
        encoding="utf-8")
    config = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}

    newproject.write_gitignore(destination)
    (destination / ".env.example").write_text(_env_example(config), encoding="utf-8")
    presets.write(destination, config)
    appgen.generate(destination, config)

    about = headline(directory)
    lines = [f"Copied the '{example}' example into '{project_name}'"
             + (f": {about}." if about else "."),
             f"  cd {name} && synqt dev"]
    if secrets(config):
        # An example that signs people in needs a provider registered and a secret placed.
        lines.append("")
        lines.append("  It reads secrets from the environment. See .env.example and the "
                     "example's own README.md.")
    lines.append("")
    lines.append(licenses.CLIENT_GPL_WARNING)
    return "\n".join(lines)
