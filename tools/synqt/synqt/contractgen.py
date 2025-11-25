# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Turn a connect point's ``export:`` block into the ``.syn`` the compiler reads.

A connect point and the shape of what crosses it are one thing, so they are written in one
place: the point's ``export:`` in ``synqt.yaml`` holds the members, and nothing names the
contract, because the point is already named.

    connect_points:
      - name: auction
        owner: edge
        consumers: [app]
        export: |
          prop string itemName
          prop int highBid
          slot placeBid(int amount)
          signal bidRejected(string reason)

What the generator wants is a file, so one is written under ``generated/`` beside the
entity's generated main. ``generated/web/edge/Auction.syn``. Its name is the point's,
capitalized, which is also the QML type the point's server file is rooted at. Nobody edits
it. Editing the ``export:`` block rewrites it.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Dict, List, Tuple

from synqt import appmodel, writer

_HEADER = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
           "// SPDX-License-Identifier: Apache-2.0\n")

#: A `record` is a type, not a member, so it is written outside the contract even though it
#: is declared inside the same block. Lifting it here is what lets one `export:` hold both.
_RECORD = re.compile(r"^\s*record\b")

#: A line that is nothing but a name: export it as the owner already implements it. The
#: kind and the types come from reading the owner's Source, which is where they were
#: already written down, so the block says what crosses and not a second time what it is.
_BARE_NAME = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)$")


def export_text(point: Dict[str, Any]) -> str:
    """The ``export:`` block of a connect point, empty when it declares none."""
    declared = point.get("export")
    return declared if isinstance(declared, str) else ""


def has_export(point: Dict[str, Any]) -> bool:
    """Did this point declare an ``export:`` block at all (even an empty one)?"""
    return isinstance(point.get("export"), str)


def _split_comment(line: str) -> Tuple[str, int, str]:
    """One written line as (code, the column its comment starts in, the comment).

    The column is kept so that a block whose comments line up still has them lined up
    after a name has been written out into a longer line.
    """
    stripped = line.strip()
    head, marker, tail = stripped.partition("//")
    if not marker:
        return stripped, 0, ""
    return head.rstrip(), len(head), marker + tail


def bare_name(line: str) -> str:
    """The name a line exports by name alone, or "" when it spells a whole member."""
    code, _, _ = _split_comment(line)
    match = _BARE_NAME.match(code)
    return match.group(1) if match else ""


def rendered(member: Any) -> str:
    """One `infer.Member` as the contract line it stands for."""
    def pairs(items: Any) -> str:
        return ", ".join(f"{item.type} {item.name}" for item in items or ())

    if member.kind == "prop":
        return f"prop {member.type or 'var'} {member.name}"
    if member.kind == "model":
        return f"model {member.name}({pairs(member.roles)})"
    if member.kind == "signal":
        return f"signal {member.name}({pairs(member.params)})"
    returned = f"{member.type} " if member.type else ""
    return f"slot {returned}{member.name}({pairs(member.params)})"


def written_out(line: str, owner: Dict[str, Any]) -> str:
    """`line`, with a name-only export replaced by the member the owner implements.

    Left exactly as written when it already spells a member, when the owner has no such
    member, or when what the owner has was guessed rather than read. A type nobody is sure
    of is not a type to put on a wire without being asked. `check.lint_exports` is what
    turns each of those into a sentence. Here they are only not expanded.
    """
    name = bare_name(line)
    member = owner.get(name) if name else None
    if member is None or not getattr(member, "certain", False):
        return line
    _, column, comment = _split_comment(line)
    code = rendered(member)
    if not comment:
        return code
    return code + " " * max(1, column - len(code)) + comment


def contract_source(name: str, point: Dict[str, Any],
                    owner: Dict[str, Any] | None = None) -> str:
    """The ``.syn`` text for one connect point's contract.

    Members go inside ``contract <Name> { ... }``; a ``record`` line goes above it, because
    a record is a type the members use rather than a member itself. Comments and blank
    lines are kept as written. The block is the author's, and this only puts a frame
    around it.

    `owner` is what the owner's Source implements (:func:`synqt.infer.owner_members`),
    which is what a line naming a member and nothing else is written out from.
    """
    records: List[str] = []
    members: List[str] = []
    for written in export_text(point).splitlines():
        line = written_out(written, owner) if owner else written
        if _RECORD.match(line):
            records.append(line.strip())
        elif line.strip():
            members.append("    " + line.strip())
        else:
            members.append("")
    while members and not members[-1]:
        members.pop()
    lines = [_HEADER.rstrip("\n"),
             f"// Generated by synqt from the '{point.get('name')}' connect point in "
             "synqt.yaml. Do not edit.",
             ""]
    if records:
        lines += records + [""]
    lines += [f"contract {name} {{"] + members + ["}"]
    return "\n".join(lines) + "\n"


def implemented_by_owner(project_dir: os.PathLike[str] | str, config: Dict[str, Any],
                         point: Dict[str, Any]) -> Dict[str, Any]:
    """What the owner of `point` implements, for writing a name-only export out.

    A thin door onto :func:`synqt.infer.owner_members`, opened here because everything
    that turns an ``export:`` block into a contract comes through this module and `infer`
    reads this one at import time. Empty when the project's QML cannot be read at all,
    which leaves every line exactly as written.
    """
    from synqt import infer  # here, because infer reads this module at import time

    try:
        return infer.owner_members(project_dir, config, point)
    except OSError:
        return {}


def resolved_source(project_dir: os.PathLike[str] | str, config: Dict[str, Any],
                    point: Dict[str, Any]) -> str:
    """The ``.syn`` text for `point`, with every name-only export written out."""
    return contract_source(appmodel.contract_of(point), point,
                           implemented_by_owner(project_dir, config, point))


def write_contracts(project_dir: os.PathLike[str] | str,
                    config: Dict[str, Any]) -> List[str]:
    """Write every app connect point's contract under ``generated/``.

    Returns the project-relative paths this owns, written or already current. A framework
    connect point is skipped. Its contract ships in the runtime library that owns it, and
    the project has no ``export:`` to write it from.
    """
    root = Path(project_dir)
    owners = {str(entity.get("name") or ""): entity
              for entity in appmodel.entities(config)}
    written: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        owner = owners.get(str(point.get("owner") or ""))
        contract = appmodel.contract_of(point)
        if owner is None or not contract:
            continue
        relative = appmodel.contract_path(owner, contract)
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        writer.write_if_changed(target, resolved_source(root, config, point))
        written.append(relative)
    return written
