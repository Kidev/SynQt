# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt create``: the interactive front end to ``synqt new``.

Two commands rather than one flag. ``synqt new`` is the scriptable one: every answer is
a flag, it reads nothing from the terminal, and it behaves identically in a shell, in a
Makefile and in CI. ``synqt create`` asks the same questions out loud and then calls it.

Splitting them is deliberate. A single command that prompts when it has a terminal and
silently picks defaults when it does not is two behaviors wearing one name: the CI run
takes a path nobody watched it take, and the difference only shows up in the generated
project. Here the name says which you get, and `create` refuses to run without a
terminal rather than quietly becoming `new`.

The questions are the security-relevant ones and nothing else. There is no question
about the origin model. A scaffolded project is same-origin, which is the only shape
whose session cookie is first-party (see the "Serving the client from another origin"
section of docs/project-layout-and-config.md). Offering a menu item is how someone picks
the other one without reading what it costs.
"""

from __future__ import annotations

import os
import sys
from typing import Any, Dict, List, Optional, Sequence, TextIO, Tuple

from . import addauth, addentity, appmodel, newproject


class CreateError(Exception):
    """A question could not be asked, or was answered with something impossible."""


# The entity types worth starting a project with, in the order they are offered.
# `client` and `web_edge` are types too and are not here. A project has one of each and
# `synqt new` has already written them.
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
        # EOF part-way through. The stream ended, so there is no answer coming and no
        # sound way to invent one. Better to stop than to scaffold half a decision.
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
    """Which identity provider to prime, or None for no authentication yet.

    No is the default because no insecure auth state is the default. A project with no
    identity configured cannot have a half-configured one.
    """
    providers = ", ".join(addauth.TEMPLATED_PROVIDERS)
    out.write(f"\nAdd authentication now? Templated providers: {providers}.\n"
              "  Leave empty for none; `synqt add auth <provider>` adds it later.\n")
    answer = _prompt("Provider", default="none", out=out, source=source).lower()
    if answer in ("", "none", "no", "n"):
        return None
    if answer not in addauth.TEMPLATED_PROVIDERS:
        # Not a refusal. A provider with no template is configured by hand, and saying so
        # is more useful than rejecting the answer.
        out.write(f"  '{answer}' has no template; scaffolding it as a generic OIDC provider.\n")
    return answer


def ask_entities(out: TextIO, source: TextIO) -> List[Tuple[str, str]]:
    """The starting entities beyond the client and the edge, each as (name, type).

    Two questions per entity, name first. The name is asked for and never derived from
    the type. It is what the entity's folder, its own QML file and its accessor in every
    consumer are built from, so it is the author's word or it is a word they have to
    change later. Asking the two separately is also why this reads nothing like the
    `<name>:<type>` pair a single flag would have needed.
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
    """Ask every question and return what `newproject.scaffold` needs.

    Separated from the scaffolding so the questions can be tested without writing a
    project, and so a caller that already knows the name skips that one.
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

    # Refuse rather than degrade. A `synqt create` in a pipeline that answered its own
    # questions from defaults would produce a project nobody chose, and the failure would
    # surface much later as a missing entity.
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
