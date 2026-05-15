# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The example systems the CLI ships, and ``synqt new <name> --example <example>``.

An example is a whole project, not a snippet. The same `synqt.yaml`, the same entity
folders and the same QML a hand-built one has, which is why starting from one is a copy and
not a template expansion. What the copy changes is the project's name, and what it adds is
the two files a checkout of an example does not carry because the repository already has
them elsewhere (a `.gitignore` and a `.env.example`).

They are read out of the framework root, the same directory `src/` and `cmake/` come from,
so a checkout uses the examples being edited and an installed wheel uses the copy vendored
inside it (`tools/synqt/_build_backend.py` puts them there).
"""

from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import Any, Dict, List, Tuple

import yaml

from . import appgen, licenses, newproject, presets, yamledit


class ExampleError(Exception):
    """A bad example name or an unusable examples directory, surfaced without a traceback."""


# Written from one machine's run and meaningless on another, or somebody's secrets. The
# vendoring step drops the same names, so this matters only in a checkout, where a developer
# who has run `synqt dev` in `examples/gavel` has all of them lying about.
_SKIP = {"build", "generated", "CMakeUserPresets.json", ".env", "certs", "__pycache__"}


def root() -> Path:
    """Where the shipped examples live.

    `appmodel.framework_root` answers the same question for `src/` and `cmake/` and is not
    reused, for one reason. It refuses a copy unpacked into a temporary directory, because a
    path that disappears at exit must never be baked into a project's CMake. Copying a
    directory out of one is fine, so the frozen binary can offer examples even though it can
    never be a SYNQT_ROOT.
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
    """One line saying what an example is, read off the title of its own README.

    The READMEs are written `# stall: a storefront with edge-delivered campaigns`, so the
    half after the colon is the sentence and the half before it is the name already in the
    first column. Nothing is written down twice. An example whose README says something else
    is listed by whatever its title says.
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
    """A `.env.example` naming every secret the copied project reads from its environment.

    An example in the repository has none, because the repository is not where anybody runs
    one. A copy is, and a project whose edge will not start until `GITHUB_CLIENT_SECRET` is
    set should say so in a file rather than in a stack trace.
    """
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
    if destination.exists() and any(destination.iterdir()):
        raise newproject.NewProjectError(f"{destination} already exists and is not empty")
    if destination.exists():
        # copytree will not write into a directory that is already there, and an empty one
        # is what `mkdir app && cd app` leaves. Taking it as the target rather than refusing
        # it keeps the two `synqt new` shapes behaving the same way.
        destination.rmdir()
    _copy(directory, destination)

    config_path = destination / "synqt.yaml"
    # Edited rather than re-serialised, because an example's synqt.yaml is a file somebody
    # is meant to read: it is commented throughout, and yaml.safe_dump would hand the reader
    # a version of their new project with every one of those comments deleted.
    config_path.write_text(
        yamledit.set_scalar(config_path.read_text(encoding="utf-8"), "project.name", name),
        encoding="utf-8")
    config = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}

    newproject.write_gitignore(destination)
    (destination / ".env.example").write_text(_env_example(config), encoding="utf-8")
    presets.write(destination, config)
    appgen.generate(destination, config)

    about = headline(directory)
    lines = [f"Copied the '{example}' example into '{name}'"
             + (f": {about}." if about else "."),
             f"  cd {name} && synqt dev"]
    if secrets(config):
        # Named here rather than left to the first failed start. An example that signs
        # people in needs a provider registered and a secret placed, and neither is
        # something a copy can do for somebody.
        lines.append("")
        lines.append("  It reads secrets from the environment. See .env.example and the "
                     "example's own README.md.")
    lines.append("")
    lines.append(licenses.CLIENT_GPL_WARNING)
    return "\n".join(lines)
