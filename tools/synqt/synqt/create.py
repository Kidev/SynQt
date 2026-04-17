# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt create``: the interactive front end to ``synqt new``.

``synqt new`` takes every answer as a flag and never reads the terminal. ``synqt create``
asks the questions and then calls it, and refuses to run without a terminal. It asks only
the security-relevant questions. There is no origin model question: a scaffolded project is
same-origin (see "Serving the client from another origin" in
docs/project-layout-and-config.md).
"""

from __future__ import annotations

import os
import sys
from typing import Any, Dict, List, Optional, Sequence, TextIO, Tuple

from . import addauth, addentity, appmodel, newproject


class CreateError(Exception):
    """A question could not be asked, or was answered with something impossible."""


# The entity types offered, in order. `client` and `web_edge` are written by `synqt new`.
_STARTING_TYPES: Sequence[str] = ("relational", "cache", "document", "api", "jobs",
                                  "service")

_TYPE_BLURB: Dict[str, str] = {
    "relational": "durable rows behind the edge (SQLite by default)",
    "cache": "bounded in-memory key-value, evicts under pressure",
    "document": "schemaless documents behind the edge",
    "api": "outbound HTTP to third-party APIs, over verified TLS",
    "jobs": "timers and a bounded background queue, internal only",
    "service": "no engine and no browser-facing side; just its own QML",
}


def _prompt(question: str, *, default: str, out: TextIO, source: TextIO) -> str:
    """Ask once and return the trimmed answer, or `default` when the answer is empty."""
    out.write(f"{question} [{default}]: ")
    out.flush()
    answer = source.readline()
    if answer == "":
        # EOF mid-question: stop rather than guess.
        raise CreateError("input ended before the questions were answered")
    answer = answer.strip()
    return answer if answer else default


def ask_name(out: TextIO, source: TextIO, *, suggested: str = "my-app") -> str:
    """The project name, which is also the directory `create` will write into."""
    while True:
        name = _prompt("Project name", default=suggested, out=out, source=source)
        if name and "/" not in name and "\\" not in name and not name.startswith("."):
            return name
        out.write("  A project name is one directory component, and not a hidden one.\n")


def ask_auth(out: TextIO, source: TextIO) -> Optional[str]:
    """Which identity provider to prime, or None. No authentication is the default."""
    providers = ", ".join(addauth.TEMPLATED_PROVIDERS)
    out.write(f"\nAdd authentication now? Templated providers: {providers}.\n"
              "  Leave empty for none; `synqt add auth <provider>` adds it later.\n")
    answer = _prompt("Provider", default="none", out=out, source=source).lower()
    if answer in ("", "none", "no", "n"):
        return None
    if answer not in addauth.TEMPLATED_PROVIDERS:
        # A provider with no template is configured by hand; say so.
        out.write(f"  '{answer}' has no template; scaffolding it as a generic OIDC provider.\n")
    return answer


def ask_entities(out: TextIO, source: TextIO) -> List[Tuple[str, str]]:
    """The starting entities beyond the client and the edge, each as (name, type). The name is
    asked for, never derived from the type.
    """
    out.write("\nStarting entities beyond the client and the web edge.\n")
    for entity_type in _STARTING_TYPES:
        out.write(f"  {entity_type:<12} {_TYPE_BLURB[entity_type]}\n")
    out.write("  Name one at a time; leave the name empty to stop. "
              "`synqt add entity` adds one later.\n")

    chosen: List[Tuple[str, str]] = []
    while True:
        name = _prompt("Entity name (empty to finish)", default="", out=out,
                       source=source).strip()
        if not name:
            return chosen
        if name in [already for already, _ in chosen]:
            raise CreateError(f"two starting entities are both called '{name}'")
        entity_type = _prompt(f"Type for '{name}'", default=appmodel.PLAIN_TYPE, out=out,
                              source=source).strip().lower()
        if entity_type not in addentity.TYPES:
            known = ", ".join(_STARTING_TYPES)
            raise CreateError(f"unknown entity type '{entity_type}' (choose from: {known})")
        chosen.append((name, entity_type))


def answers(out: TextIO, source: TextIO, *, name: Optional[str] = None) -> Dict[str, Any]:
    """Ask every question and return what `newproject.scaffold` needs. Separate, for testing
    and so a known name is not asked.
    """
    out.write("Creating a SynQt project. Press Enter to take the default.\n\n")
    resolved = name if name else ask_name(out, source)
    return {
        "name": resolved,
        "auth": ask_auth(out, source),
        "entities": ask_entities(out, source),
    }


def create(parent_dir: os.PathLike[str] | str, *, name: Optional[str] = None,
           out: Optional[TextIO] = None, source: Optional[TextIO] = None,
           interactive: Optional[bool] = None) -> str:
    """Ask, then scaffold. Returns what `synqt new` would have printed."""
    stream_out = out if out is not None else sys.stdout
    stream_in = source if source is not None else sys.stdin

    # Refuse without a terminal rather than take defaults.
    if interactive is None:
        interactive = bool(getattr(stream_in, "isatty", lambda: False)())
    if not interactive:
        raise CreateError(
            "synqt create asks questions and needs a terminal. For a script or CI, "
            "use `synqt new <name> [--auth <provider>]` and then "
            "`synqt add entity <name> --type <type>` for each entity.")

    chosen = answers(stream_out, stream_in, name=name)
    stream_out.write("\n")
    return newproject.scaffold(parent_dir, chosen["name"], auth=chosen["auth"],
                               starting=chosen["entities"])
