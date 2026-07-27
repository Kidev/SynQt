# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the effective configuration from its layers.

`docs/project-layout-and-config.md` states the order, and this module is the only place
that implements it. Later layers override earlier ones, key by key:

1. Framework defaults. These are not a table here: each reader carries its own
   (``entity.get("targets", ["wasm"])``), so a default lives next to the code that
   depends on it and cannot drift from it.
2. ``synqt.yaml``, the project topology.
3. ``synqt.<profile>.yaml``, selected with ``--profile``. Same schema, carrying only the
   keys it changes.
4. ``SYNQT_<SECTION>_<KEY>`` environment variables, for CI and containers.
5. CLI flags, which the CLI applies itself after this module has run.

Two properties are worth stating outright, because both are load-bearing:

*A profile changes and adds; it never removes.* Merging a list by deleting from it would
be an invisible way to drop an entity, a consumer, or a connect point, and dropping a
consumer from a list is a security change. What a profile can do is override the entries
it names.

*Layer 4 cannot invent a section.* An environment variable is applied only when its first
token names a section the configuration already declares. That keeps ``SYNQT_ROOT``,
``SYNQT_EDGE_URL``, ``SYNQT_TEST_PG_HOST`` and the rest of the SynQt runtime's own
namespace out of the topology, and it means a container cannot conjure a section that no
reviewed ``synqt.yaml`` ever mentioned.

Neither layer is a way in for a secret. Validation runs on the *resolved* configuration,
so a password arriving from a profile file or from ``SYNQT_...`` is rejected by the same
rule that rejects one typed into ``synqt.yaml``: it has to be an ``env:`` reference the
entity resolves for itself.
"""

from __future__ import annotations

import copy
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Tuple

import yaml

ENV_PREFIX = "SYNQT_"


class ConfigError(Exception):
    """A layered-configuration error surfaced to the CLI (no traceback for the user)."""


@dataclass(frozen=True)
class Resolved:
    """The effective configuration plus a record of where it came from.

    ``sources`` is for the human: a build that behaves differently on CI than on a laptop
    is nearly always a layer the reader did not know was applied, so every layer beyond
    the base file names itself.
    """

    config: Dict[str, Any]
    sources: List[str] = field(default_factory=list)


def profile_filename(profile: str) -> str:
    return f"synqt.{profile}.yaml"


def config_filenames(profile: Optional[str] = None) -> Tuple[str, ...]:
    """The file names that make up the configuration, base first.

    `synqt dev` watches these: with a profile active, editing the profile file is editing
    the topology, and a watcher that only knew about ``synqt.yaml`` would keep serving the
    old wiring with no sign that it had missed the save.
    """
    if profile:
        return ("synqt.yaml", profile_filename(profile))
    return ("synqt.yaml",)


def _is_named_list(value: Any) -> bool:
    """A list of mappings that all carry a ``name``: ``entities`` and ``connect_points``.

    These are the two lists SynQt keys by name, which is what makes merging them key by
    key meaningful rather than a guess about list positions.
    """
    return (isinstance(value, list) and bool(value)
            and all(isinstance(item, dict) and item.get("name") for item in value))


def merge(base: Any, override: Any) -> Any:
    """Layer ``override`` onto ``base``, key by key.

    Mappings merge recursively. A name-keyed list (``entities``, ``connect_points``)
    merges entry by entry on ``name``, keeping the base order and appending entries the
    base did not have, so a profile can retune one entity without restating the topology.
    Every other list, and every scalar, is replaced outright: a plain list such as
    ``consumers`` or ``scopes.order`` is a single value whose order and membership are the
    point, and half-merging one would be a silent way to change who may reach what.
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
    """The longest existing key of ``node`` that ``remainder`` starts with.

    Longest wins so ``SYNQT_BUILD_DESKTOP_EDGE_URL`` resolves against the structure that
    is actually there (``build.desktop.edge_url``) instead of against a reading of the
    underscores (``build.desktop_edge_url``), which no amount of naming convention could
    disambiguate on its own.
    """
    candidates = [key for key in node
                  if isinstance(key, str)
                  and (remainder == key or remainder.startswith(f"{key}_"))]
    return max(candidates, key=len) if candidates else None


def _target_path(config: Mapping[str, Any], name: str) -> Optional[List[str]]:
    """Resolve ``build_desktop_edge_url`` to ``["build", "desktop", "edge_url"]``.

    Returns None when the variable does not address a mapping key inside an existing
    section. That covers three cases, all of them deliberate: the first token names no
    section (which is how the SynQt runtime's own ``SYNQT_ROOT``, ``SYNQT_EDGE_URL`` and
    ``SYNQT_TEST_*`` stay out of the topology), the path runs into a list (``entities``
    and ``connect_points`` are keyed by name in a file, not addressable from a variable),
    or it runs into a scalar it would have to restructure.
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
    """Read an environment string as the type the configuration already uses there.

    Type-directed rather than "parse it as YAML and see", because YAML would turn the
    perfectly good project name ``no`` into ``False`` and the version ``1.10`` into the
    float ``1.1``. Where the key is new and there is no type to follow, YAML is the
    fallback, so a list or a number still arrives as one.
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

    Sorted by variable name so two runs of the same environment resolve identically even
    where two variables address the same key.
    """
    environment = os.environ if env is None else env
    merged = copy.deepcopy(config)
    applied: List[str] = []
    for variable in sorted(environment):
        if not variable.startswith(ENV_PREFIX) or variable == ENV_PREFIX:
            continue
        path = _target_path(merged, variable[len(ENV_PREFIX):].lower())
        # The documented form is SYNQT_<SECTION>_<KEY>: a key inside a section, never a
        # bare section. Honoring a bare one would let a single variable replace the whole
        # of `entities` or `security` with one scalar, which no deployment wants and no
        # reviewer would spot.
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

    A missing ``synqt.yaml`` is an empty configuration unless ``required``: several
    commands are expected to run outside a project and say so in their own words. A
    missing *profile* file is always an error, because asking for a profile that is not
    there means the build about to run is not the one that was asked for.
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
    return Resolved(config=config, sources=sources)


def load(project_dir: os.PathLike[str] | str, *, profile: Optional[str] = None,
         env: Optional[Mapping[str, str]] = None,
         required: bool = False) -> Dict[str, Any]:
    """`resolve` when the caller wants only the configuration."""
    return resolve(project_dir, profile=profile, env=env, required=required).config
