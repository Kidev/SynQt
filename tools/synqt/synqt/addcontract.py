# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add connect-point``: scaffold the typed boundary.

Writes the point into ``synqt.yaml`` with a starter ``export:`` block, and the owner-side
Source beside the owner files.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

from synqt import appmodel, qmlscan, yamledit

#: The starter `export:` of a new connect point. Only declared model roles reach a consumer,
#: and props are READPUSH.
_EXPORT_TEMPLATE = """prop int count                    // owner writes, consumers read
model rows(int id, string[200] text)   // only these roles cross to consumers
slot add(string[200] text)        // a consumer -> owner request, so authorize Caller
signal changed()                  // the owner notifies consumers
"""

_SOURCE_TEMPLATE = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The connect point the "{point}" entity exports. The `export:` block on that point in
// synqt.yaml declares what crosses it; nothing undeclared reaches a consumer. A slot call
// arrives here with `Caller` set to the caller: authorize them first, then act. Checks in a
// consumer's UI do not protect anything; this file does.
{contract} {{
    id: root
{declared}}}
"""

# Names SynQt puts in the QML scope of every entity: the Source context accessors and the
# types registered in the SynQt module. A file in the entity directory becomes a type that
# wins over an import, so `Session.qml` would shadow the accessor.
ALWAYS_RESERVED = frozenset({
    "App", "Caller", "Client", "EntityTest", "Graphics", "IdentityMapping", "PageSeed",
    "Router", "Server", "Session",
}) | frozenset(appmodel.UNIVERSAL_HELPERS)


class AddContractError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def reserved_for(entity_type: Optional[str] = None,
                 entity: Optional[Dict[str, Any]] = None) -> frozenset:
    """The names that cannot be used inside this entity.

    The always-reserved set plus the helpers this entity has: the one its type installs
    (`appmodel.TYPE_HELPERS`) and those its `network:` block grants (`Http`, `Api`). `Log`
    is always reserved (`appmodel.UNIVERSAL_HELPERS`). Matches what `EntityRuntime`
    installs.
    """
    reserved = set(ALWAYS_RESERVED)
    if entity_type is not None:
        helper = appmodel.TYPE_HELPERS.get(entity_type)
        if helper:
            reserved.add(helper)
    if entity is not None:
        reserved.update(appmodel.network_helpers(entity))
    return frozenset(reserved)


def check_qml_name(name: str, *, entity_type: Optional[str] = None,
                   entity: Optional[Dict[str, Any]] = None) -> str:
    """`name` back, or an error saying why it cannot name a QML type here.

    A contract name is also a file name and a type name (``Items.syn``, ``Items``,
    ``ItemsSourceHelper``). `entity_type` decides which type helper name is taken; omitted,
    only the always-reserved names are.
    """
    if not name or not name.isascii() or not name.isidentifier() or not name[0].isupper():
        raise AddContractError(
            f"'{name}' cannot name a QML type; use a name that starts with a capital "
            "letter and holds only letters, digits and underscores (for example Items)")
    if name in reserved_for(entity_type, entity):
        where = (f"every entity of type '{entity_type}'" if name not in ALWAYS_RESERVED
                 else "every entity")
        raise AddContractError(
            f"'{name}' is what SynQt calls one of the helpers the QML of {where} uses, and "
            f"a {name}.qml of your own would shadow it wherever it is called; pick another "
            "name")
    return name


def owner_entity(project_dir: os.PathLike[str] | str, owner: str) -> Dict[str, Any]:
    """The entity block for an owner named in a command, read from the project."""
    config_path = Path(project_dir) / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        config = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}
    for entity in config.get("entities") or []:
        if isinstance(entity, dict) and entity.get("name") == owner:
            return entity
    raise AddContractError(f"unknown entity '{owner}'")


def _base_type(written: str) -> str:
    """A contract type without its bracketed size: `string[120]` is a string. The generated
    code enforces the size; QML has no sized types.
    """
    return str(written or "").split("[")[0].strip()


def _declaration(member: Dict[str, Any]) -> str:
    """One contract member as the QML line that declares it.

    Parameters and return types use the annotated form (`amount: int`). An unwritten body is
    `return;` and a signal with no parameters has no parentheses, both as qmlformat writes
    them.
    """
    kind = member.get("kind")
    name = member.get("name") or ""
    params = ", ".join(f"{p.get('name')}: {_base_type(p.get('type'))}"
                       for p in member.get("params") or [])
    if kind == "prop":
        return f"    property {_base_type(member.get('type')) or 'var'} {name}"
    if kind == "signal":
        return f"    signal {name}({params})" if params else f"    signal {name}"
    returns = f": {_base_type(member['type'])}" if member.get("type") else ""
    return f"    function {name}({params}){returns} {{\n        return;\n    }}"


def declarations_for(members: Optional[List[Dict[str, Any]]]) -> str:
    """The declarations for a contract's members inside the owner Source.

    Properties, then signals, then functions, with a blank line between groups and functions
    (projects ship `NormalizeOrder=false`). A model has no QML declaration and is skipped;
    the generated Source helper publishes it.
    """
    kept = [member for member in members or []
            if member.get("kind") != "model" and member.get("name")]
    groups: List[str] = []
    for kind in ("prop", "signal"):
        written = [_declaration(member) for member in kept if member.get("kind") == kind]
        if written:
            groups.append("\n".join(written))
    groups.extend(_declaration(member) for member in kept
                  if member.get("kind") not in ("prop", "signal"))
    return "\n\n".join(groups)


def source_stub(contract: str, point: str,
                members: Optional[List[Dict[str, Any]]] = None) -> str:
    """An owner-side Source rooted at the contract type, declaring the contract's members."""
    declared = declarations_for(members)
    return _SOURCE_TEMPLATE.format(contract=contract, point=point,
                                   declared=f"\n{declared}\n" if declared else "")


def untouched_scaffold(text: str, entity: Dict[str, Any]) -> bool:
    """Whether this file is exactly what a scaffolder wrote. Only then may exporting a point
    rewrite it.
    """
    from . import addentity, newproject  # here: newproject reaches addentity at import time

    name = str(entity.get("name") or "")
    return text in {newproject.entity_singleton(name),
                    addentity.entity_qml(appmodel.entity_type(entity), name)}


def write_source(project_dir: os.PathLike[str] | str, owner: Dict[str, Any], contract: str, *,
                 point: str, path: Optional[str] = None,
                 members: Optional[List[Dict[str, Any]]] = None) -> Optional[str]:
    """Write the owner-side Source for a connect point, unless the author has one.

    Returns the project-relative path when written, None when the file was left alone.
    """
    relative = path or appmodel.source_path(owner, contract)
    target = Path(project_dir) / relative
    if target.exists() and not untouched_scaffold(
            target.read_text(encoding="utf-8", errors="replace"), owner):
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source_stub(contract, point, members), encoding="utf-8")
    return relative


def _root_note(project_dir: os.PathLike[str] | str, owner: Dict[str, Any],
               contract: str) -> List[str]:
    """A note about a Source file that exists but is not rooted at its contract, such as the
    `synqt add entity` stub rooted at QtObject. `synqt check` refuses it too.
    """
    relative = appmodel.source_path(owner, contract)
    source = Path(project_dir) / relative
    root = qmlscan.root_type(source.read_text(encoding="utf-8", errors="replace"))
    if root is None or root == contract:
        return []
    return [f"  - {relative} is rooted at '{root}'. A connect point Source has to be "
            f"rooted at '{contract}'; change it, or point this connect point at "
            "another file with 'server:'."]


def scaffold_connect_point(project_dir: os.PathLike[str] | str, owner: str, *,
                           consumers: List[str]) -> str:
    """Add the connect point `owner` exports, and the Source that implements it.

    `edge` exports the `Edge` contract from `web/edge/Edge.qml`. An entity that already has
    a point is told to extend its `export:` block.
    """
    contract = appmodel.contract_of({"owner": owner})
    owning = owner_entity(project_dir, owner)
    check_qml_name(contract, entity_type=appmodel.entity_type(owning), entity=owning)
    config_path = Path(project_dir) / "synqt.yaml"
    if not config_path.exists():
        raise AddContractError("no synqt.yaml (run 'synqt new' first)")
    config: Dict[str, Any] = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}

    entities = {e.get("name") for e in config.get("entities", []) if isinstance(e, dict)}
    if owner not in entities:
        raise AddContractError(f"unknown owner entity '{owner}'")
    for consumer in consumers:
        if consumer not in entities:
            raise AddContractError(f"unknown consumer entity '{consumer}'")

    connect_points: List[Dict[str, Any]] = config.get("connect_points") or []
    if any(isinstance(cp, dict) and cp.get("owner") == owner for cp in connect_points):
        raise AddContractError(
            f"'{owner}' already has a connect point, and an entity has one. Add what you "
            f"wanted to its 'export:' block in synqt.yaml; a member for a narrower audience "
            "goes there too, gated as '<scope> slot ...'")

    # Spliced into the text, keeping the author's comments and formatting.
    block: Dict[str, Any] = {"owner": owner, "consumers": consumers,
                             "export": _EXPORT_TEMPLATE}
    config_path.write_text(yamledit.append_item(
        config_path.read_text(encoding="utf-8"), "connect_points", block), encoding="utf-8")
    owning = owner_entity(project_dir, owner)
    written = write_source(project_dir, owning, contract, point=owner)
    steps = [f"Added the connect point '{owner}' exports (contract {contract}, "
             f"consumers {', '.join(consumers) or 'none'}). "
             "Deny-by-default: only listed consumers may acquire it."]
    if written:
        steps.append(f"  - {written} is the entity and now exports this point, rooted at "
                     f"'{contract}'. Fill in the slots there and authorize Caller in every "
                     "one of them.")
    else:
        steps.extend(_root_note(project_dir, owning, contract)
                     or [f"  - {appmodel.source_path(owning, contract)} is already there; "
                         "authorize Caller in every slot it implements."])
    steps.append("  - What crosses it is the point's 'export:' block in synqt.yaml; "
                 "the starter one there is an example to replace.")
    return "\n".join(steps)
