# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`.dev-identities`: the named people a developer signs in as.

The scope picker (`synqt dev --identity-picker`) offers the declared scopes; this file adds
named people:

.. code-block:: yaml

    - email: alice@example.com
      scope: admin
    - email: bob@example.com
      scope: user

`synqt dev` reads it here, against the project scopes, and passes the valid entries to the
edge as command-line values. A bad entry is dropped and reported on the picker page
(`--dev-identity-problem`), never fatal.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

import yaml

FILE_NAME = ".dev-identities"

#: An entry as the edge takes it: `<scope>=<email>`. A scope is a bare identifier, so the
#: first `=` separates.
Entry = Tuple[str, str]


def path_of(project_dir: os.PathLike[str] | str) -> Path:
    return Path(project_dir) / FILE_NAME


def read(project_dir: os.PathLike[str] | str,
         scope_order: Sequence[str]) -> Tuple[List[Entry], List[str]]:
    """Return the usable entries and one sentence per dropped entry, naming it."""
    file = path_of(project_dir)
    if not file.exists():
        return [], []

    try:
        document = yaml.safe_load(file.read_text()) or []
    except yaml.YAMLError as error:
        # A parse error gives one message for the whole file.
        return [], [f"{FILE_NAME} is not valid YAML and was ignored entirely: {error}"]

    if not isinstance(document, list):
        return [], [f"{FILE_NAME} must be a list of entries, and this one is a "
                    f"{type(document).__name__}; the whole file was ignored"]

    entries: List[Entry] = []
    problems: List[str] = []
    for index, item in enumerate(document, start=1):
        problem = _problem_with(index, item, scope_order)
        if problem:
            problems.append(problem)
            continue
        entries.append((str(item["scope"]), str(item["email"])))
    return entries, problems


def _problem_with(index: int, item: Any, scope_order: Sequence[str]) -> str:
    where = f"{FILE_NAME} entry {index}"
    if not isinstance(item, dict):
        return f"{where} is a {type(item).__name__}, not an email/scope pair; ignored"

    email = item.get("email")
    scope = item.get("scope")
    if not isinstance(email, str) or not email.strip():
        return f"{where} names no email; ignored"
    if not isinstance(scope, str) or not scope.strip():
        return f"{where} ({email}) names no scope; ignored"

    # The address becomes a command-line value, an identity field and page text, so newlines
    # and control characters are refused.
    if any(character.isspace() or ord(character) < 0x20 for character in email):
        return f"{where} ({email!r}) has whitespace or a control character in its email; ignored"

    if scope not in scope_order:
        declared = ", ".join(scope_order) or "none"
        return (f"{where} ({email}) names scope '{scope}', which this project does not "
                f"declare; declared: {declared}. Ignored")
    return ""


def arguments(entries: Sequence[Entry], problems: Sequence[str]) -> List[str]:
    """The flags the edge takes, in the order a reader of the command line would want them."""
    flags: List[str] = []
    for scope, email in entries:
        flags += [f"--dev-identity={scope}={email}"]
    for problem in problems:
        flags += [f"--dev-identity-problem={problem}"]
    return flags


def for_project(project_dir: os.PathLike[str] | str,
                config: Dict[str, Any]) -> Tuple[List[str], List[str]]:
    """Read the file for one project and return (flags, problems). `synqt dev` also prints the
    problems at startup.
    """
    from . import appmodel  # local: appmodel imports nothing from here, and this keeps it so

    entries, problems = read(project_dir, appmodel.scope_vocab(config))
    return arguments(entries, problems), problems
