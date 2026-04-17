# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the effective configuration from its layers.

The order is stated in `docs/project-layout-and-config.md` and implemented only here. Later
layers override earlier ones, key by key:

1. Framework defaults, kept by each reader (``entity.get("targets", ["wasm"])``).
2. ``synqt.yaml``, the project topology.
3. ``synqt.<profile>.yaml``, selected with ``--profile``, carrying only the keys it changes.
4. ``SYNQT_<SECTION>_<KEY>`` environment variables, for CI and containers.
5. CLI flags, applied by the CLI after this module.

A profile changes and adds, never removes, since removing a consumer is a security change.
An environment variable applies only when its first token names a section the configuration
already has, which keeps ``SYNQT_ROOT``, ``SYNQT_EDGE_URL`` and the like out of the
topology. Validation runs on the resolved configuration, so a secret from any layer must be
an ``env:`` reference.
"""

from __future__ import annotations

import copy
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Tuple

import yaml

from . import appmodel

ENV_PREFIX = "SYNQT_"


class ConfigError(Exception):
    """A layered-configuration error surfaced to the CLI (no traceback for the user)."""


@dataclass(frozen=True)
class Resolved:
    """The effective configuration plus a record of where it came from. Every layer beyond the
    base file is named in ``sources``.
    """

    config: Dict[str, Any]
    sources: List[str] = field(default_factory=list)


def profile_filename(profile: str) -> str:
    return f"synqt.{profile}.yaml"


def config_filenames(profile: Optional[str] = None) -> Tuple[str, ...]:
    """The file names that make up the configuration, base first. `synqt dev` watches them all."""
    if profile:
        return ("synqt.yaml", profile_filename(profile))
    return ("synqt.yaml",)


def _is_named_list(value: Any) -> bool:
    """A list of mappings that all carry a ``name``: ``entities`` and ``connect_points``,
    merged by name.
    """
    return (isinstance(value, list) and bool(value)
            and all(isinstance(item, dict) and item.get("name") for item in value))


def merge(base: Any, override: Any) -> Any:
    """Layer ``override`` onto ``base``, key by key.

    Mappings merge recursively. A name-keyed list merges entry by entry on ``name``, keeping
    the base order and appending new entries. Every other list and scalar is replaced
    outright, so ``consumers`` or ``scopes.order`` is never half-merged.
    """
    if isinstance(base, dict) and isinstance(override, dict):
        merged = dict(base)
        for key, value in override.items():
            merged[key] = merge(merged[key], value) if key in merged else value
        return merged
    if _is_named_list(base) and _is_named_list(override):
        by_name = {item["name"]: item for item in base}
        ordered = [item["name"] for item in base]
        for item in override:
            name = item["name"]
            if name in by_name:
                by_name[name] = merge(by_name[name], item)
            else:
                by_name[name] = item
                ordered.append(name)
        return [by_name[name] for name in ordered]
    return override


def _read(path: Path) -> Dict[str, Any]:
    try:
        loaded = yaml.safe_load(path.read_text()) or {}
    except yaml.YAMLError as error:
        raise ConfigError(f"{path.name}: {error}") from error
    if not isinstance(loaded, dict):
        raise ConfigError(f"{path.name}: expected a mapping at the top level")
    return loaded


def _longest_key(node: Mapping[str, Any], remainder: str) -> Optional[str]:
    """The longest existing key of ``node`` that ``remainder`` starts with, so
    ``SYNQT_BUILD_DESKTOP_EDGE_URL`` resolves to ``build.desktop.edge_url``.
    """
    candidates = [key for key in node
                  if isinstance(key, str)
                  and (remainder == key or remainder.startswith(f"{key}_"))]
    return max(candidates, key=len) if candidates else None


def _target_path(config: Mapping[str, Any], name: str) -> Optional[List[str]]:
    """Resolve ``build_desktop_edge_url`` to ``["build", "desktop", "edge_url"]``.

    None when the variable does not address a key inside an existing section: the first
    token names no section (``SYNQT_ROOT``, ``SYNQT_TEST_*``), the path runs into a list, or
    into a scalar.
    """
    path: List[str] = []
    node: Any = config
    remainder = name
    while remainder:
        if not isinstance(node, dict):
            return None
        match = _longest_key(node, remainder)
        if match is None:
            if not path:
                return None
            path.append(remainder)  # a new leaf under an existing section
            return path
        path.append(match)
        remainder = remainder[len(match):].lstrip("_")
        node = node.get(match)
    return path


_TRUE = {"true", "yes", "on", "1"}
_FALSE = {"false", "no", "off", "0"}


def _coerce(existing: Any, raw: str, variable: str) -> Any:
    """Read an environment string as the type the configuration already has there. Not plain
    YAML, which would read the name ``no`` as False and ``1.10`` as 1.1. YAML is the
    fallback for a new key.
    """
    if isinstance(existing, bool):
        if raw.strip().lower() in _TRUE:
            return True
        if raw.strip().lower() in _FALSE:
            return False
        raise ConfigError(f"{variable}: expected a boolean, got {raw!r}")
    if isinstance(existing, str):
        return raw
    if isinstance(existing, int):
        try:
            return int(raw.strip())
        except ValueError as error:
            raise ConfigError(f"{variable}: expected an integer, got {raw!r}") from error
    if isinstance(existing, float):
        try:
            return float(raw.strip())
        except ValueError as error:
            raise ConfigError(f"{variable}: expected a number, got {raw!r}") from error
    try:
        parsed = yaml.safe_load(raw)
    except yaml.YAMLError:
        return raw
    if isinstance(existing, list) and not isinstance(parsed, list):
        return [item.strip() for item in raw.split(",") if item.strip()]
    return raw if parsed is None and raw != "" else parsed


def _assign(config: Dict[str, Any], path: List[str], value: Any) -> None:
    node = config
    for key in path[:-1]:
        child = node.get(key)
        if not isinstance(child, dict):
            child = {}
            node[key] = child
        node = child
    node[path[-1]] = value


def apply_env(config: Dict[str, Any],
              env: Optional[Mapping[str, str]] = None) -> Tuple[Dict[str, Any], List[str]]:
    """Apply the ``SYNQT_<SECTION>_<KEY>`` layer. Returns the config and what it applied.
    Sorted by variable name, so the result is deterministic.
    """
    environment = os.environ if env is None else env
    merged = copy.deepcopy(config)
    applied: List[str] = []
    for variable in sorted(environment):
        if not variable.startswith(ENV_PREFIX) or variable == ENV_PREFIX:
            continue
        path = _target_path(merged, variable[len(ENV_PREFIX):].lower())
        # A key inside a section only; a bare section variable would replace a whole
        # section.
        if not path or len(path) < 2:
            continue
        existing: Any = merged
        for key in path:
            existing = existing.get(key) if isinstance(existing, dict) else None
        _assign(merged, path, _coerce(existing, environment[variable], variable))
        applied.append(f"{variable} -> {'.'.join(path)}")
    return merged, applied


def resolve(project_dir: os.PathLike[str] | str, *, profile: Optional[str] = None,
            env: Optional[Mapping[str, str]] = None,
            required: bool = False) -> Resolved:
    """Read the layers in order and return the effective configuration.

    A missing ``synqt.yaml`` is an empty configuration unless ``required``. A missing
    profile file is always an error.
    """
    root = Path(project_dir)
    base_path = root / "synqt.yaml"
    sources: List[str] = []
    if base_path.exists():
        config = _read(base_path)
    elif required:
        raise FileNotFoundError(f"no synqt.yaml in {project_dir}")
    else:
        config = {}

    if profile:
        profile_path = root / profile_filename(profile)
        if not profile_path.exists():
            raise ConfigError(f"no {profile_path.name} in {project_dir} "
                              f"(a --profile names a synqt.<profile>.yaml beside "
                              f"synqt.yaml)")
        config = merge(config, _read(profile_path))
        sources.append(profile_path.name)

    config, applied = apply_env(config, env)
    sources.extend(applied)
    # The default contract name, filled in once here for every reader.
    return Resolved(config=config, sources=sources)


def load(project_dir: os.PathLike[str] | str, *, profile: Optional[str] = None,
         env: Optional[Mapping[str, str]] = None,
         required: bool = False) -> Dict[str, Any]:
    """`resolve` when the caller wants only the configuration."""
    return resolve(project_dir, profile=profile, env=env, required=required).config
