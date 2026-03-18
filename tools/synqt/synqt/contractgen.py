# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Turn a connect point ``export:`` block into the ``.syn`` the compiler reads.

The members live on the point in ``synqt.yaml``; the owner names the contract::

    connect_points:
      - owner: edge
        consumers: [app]
        export: |
          prop string itemName
          prop int highBid
          slot placeBid(int amount)
          signal bidRejected(string reason)

The file is written under ``generated/`` beside the entity generated main
(``generated/web/edge/Edge.syn``), named after the owner, capitalized. Editing the
``export:`` block rewrites it.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Dict, List, Tuple

from synqt import appmodel, writer

_HEADER = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
           "// SPDX-License-Identifier: Apache-2.0\n")

#: A `record` is a type, so it is lifted outside the contract.
_RECORD = re.compile(r"^\s*record\b")

#: A line that is only a name exports the member as the owner Source implements it.
_BARE_NAME = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)$")

#: The scope gate a member may open with: `<admin>` or `<admin, auditor>`.
_GATE = re.compile(r"^(<\s*[A-Za-z_][A-Za-z0-9_]*(?:\s*,\s*[A-Za-z_][A-Za-z0-9_]*)*\s*>)\s*")


def export_text(point: Dict[str, Any]) -> str:
    """The ``export:`` block of a connect point, empty when it declares none."""
    declared = point.get("export")
    return declared if isinstance(declared, str) else ""


def has_export(point: Dict[str, Any]) -> bool:
    """Did this point declare an ``export:`` block at all (even an empty one)?"""
    return isinstance(point.get("export"), str)


def _split_comment(line: str) -> Tuple[str, int, str]:
    """One line as (code, comment column, comment). The column keeps aligned comments aligned
    after a name is written out.
    """
    stripped = line.strip()
    head, marker, tail = stripped.partition("//")
    if not marker:
        return stripped, 0, ""
    return head.rstrip(), len(head), marker + tail


def split_gate(code: str) -> Tuple[str, str]:
    """`code` as (its scope gate, what follows it). The gate is "" when it opens with none."""
    match = _GATE.match(code)
    return (match.group(1), code[match.end():]) if match else ("", code)


def bare_name(line: str) -> str:
    """The name a line exports alone, or "" when it spells a whole member. `<admin> restock`
    exports `restock`, gated.
    """
    code, _, _ = _split_comment(line)
    _, rest = split_gate(code)
    match = _BARE_NAME.match(rest)
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
    """`line` with a name-only export replaced by the member the owner implements.

    Left as written when it already spells a member, when the owner has no such member, or
    when the owner's type was guessed. `check.lint_exports` reports those.
    """
    name = bare_name(line)
    member = owner.get(name) if name else None
    if member is None or not getattr(member, "certain", False):
        return line
    written, column, comment = _split_comment(line)
    gate, _ = split_gate(written)
    code = f"{gate} {rendered(member)}" if gate else rendered(member)
    if not comment:
        return code
    return code + " " * max(1, column - len(code)) + comment


def with_inherited_gate(line: str, scope: str) -> str:
    """`line` with the connect point ``scope:`` written onto it, if it names none.

    `scope: moderator` on the point and `<admin>` on one member: moderators reach everything
    but that member. Filled in so a generated ``.syn`` is complete. Records, blank lines and
    comment lines take no gate.
    """
    if not scope:
        return line
    code, _, _ = _split_comment(line)
    if not code or _RECORD.match(line) or split_gate(code)[0]:
        return line
    indent = line[:len(line) - len(line.lstrip())]
    return f"{indent}<{scope}> {line.lstrip()}"


def contract_source(name: str, point: Dict[str, Any],
                    owner: Dict[str, Any] | None = None, *, inherit: bool = True) -> str:
    """The ``.syn`` text for one connect point contract.

    Members go inside ``contract <Name> { ... }``; ``record`` lines go above it. Comments
    and blank lines are kept. `owner` (:func:`synqt.infer.owner_members`) expands name-only
    lines.
    """
    records: List[str] = []
    members: List[str] = []
    # `inherit` is off for a reader that writes the block back out, so a `scope:` written
    # once stays once.
    inherited = str(point.get("scope") or "").strip() if inherit else ""
    for written in export_text(point).splitlines():
        line = written_out(written, owner) if owner else written
        line = with_inherited_gate(line, inherited)
        if _RECORD.match(line):
            records.append(line.strip())
        elif line.strip():
            members.append("    " + line.strip())
        else:
            members.append("")
    while members and not members[-1]:
        members.pop()
    lines = [_HEADER.rstrip("\n"),
             # A point is named by its owner.
             f"// Generated by synqt from the connect point '{point.get('owner')}' owns "
             "in synqt.yaml. Do not edit.",
             ""]
    if records:
        lines += records + [""]
    lines += [f"contract {name} {{"] + members + ["}"]
    return "\n".join(lines) + "\n"


def implemented_by_owner(project_dir: os.PathLike[str] | str, config: Dict[str, Any],
                         point: Dict[str, Any]) -> Dict[str, Any]:
    """What the owner of `point` implements, for expanding name-only exports
    (:func:`synqt.infer.owner_members`). Empty when the project QML cannot be read.
    """
    from synqt import infer  # here, because infer reads this module at import time

    try:
        return infer.owner_members(project_dir, config, point)
    except OSError:
        return {}


def resolved_source(project_dir: os.PathLike[str] | str, config: Dict[str, Any],
                    point: Dict[str, Any], *, inherit: bool = True) -> str:
    """The ``.syn`` text for `point`, with every name-only export written out.

    With `inherit` (the build) the point ``scope:`` is filled onto every member. Without it
    (the design editor) the block keeps what the author wrote.
    """
    return contract_source(appmodel.contract_of(point), point,
                           implemented_by_owner(project_dir, config, point),
                           inherit=inherit)


def write_contracts(project_dir: os.PathLike[str] | str,
                    config: Dict[str, Any]) -> List[str]:
    """Write every app connect point contract under ``generated/``. Returns the
    project-relative paths, written or current. Framework points are skipped.
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
