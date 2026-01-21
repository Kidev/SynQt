# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add connect-point``: scaffold the typed boundary.

One command, because a connect point and the shape of what crosses it are one thing: the
point is written into ``synqt.yaml`` with a starter ``export:`` block, and the owner-side
Source is written beside the owner's other files. There is nothing else to add.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

from synqt import appmodel, qmlscan, yamledit

#: The starter `export:` a new connect point is written with: the typed shape of what may
#: cross it. Only declared model roles ever reach a consumer, and props are READPUSH
#: (consumers read them, and cannot set them).
_EXPORT_TEMPLATE = """prop int count                    // owner writes, consumers read
model rows(int id, string[200] text)   // only these roles cross to consumers
slot add(string[200] text)        // a consumer -> owner request, so authorize Caller
signal changed()                  // the owner notifies consumers
"""

_SOURCE_TEMPLATE = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The connect point the "{point}" entity exports. What crosses it is the `export:` block
// on that point in synqt.yaml, and nothing undeclared ever reaches a consumer. A slot a
// consumer calls arrives here with `Caller` set to whoever called it: authorize that caller
// first, then act. This file is where the rule lives. A check in a consumer's UI is a
// courtesy, not a guard.
{contract} {{
    id: root
{declared}}}
"""

# The names SynQt puts in the QML scope of EVERY entity, whatever it is: the accessors the
# runtime installs on a Source's context and the types registered into the SynQt module. A
# file in an entity directory becomes a QML type named after the file, and a type from the
# directory wins over one from an import, so `Session.qml` beside a Source would quietly
# shadow the session accessor that Source calls. Refused where somebody picks the name
# rather than debugged where the call goes wrong.
ALWAYS_RESERVED = frozenset({
    "App", "Caller", "Client", "EntityTest", "Graphics", "IdentityMapping", "PageSeed",
    "Router", "Server", "Session",
})


class AddContractError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def reserved_for(entity_type: Optional[str] = None,
                 entity: Optional[Dict[str, Any]] = None) -> frozenset:
    """The names that cannot be used inside this entity.

    The always-reserved set, plus the helpers this entity actually has in scope: the ONE
    its type installs (`Db` in a relational entity, `Cache` in a cache entity, and so on,
    from `appmodel.TYPE_HELPERS`), and the ones its `network:` block grants (`Http` when it
    may call out, `Api` when it serves an inbound surface). `EntityRuntime` installs
    exactly these, so `Cache` is a name in scope in a cache entity and a name like any
    other everywhere else. Reserving every helper globally, which is what this used to do,
    made all of those words unusable in every entity in the project to prevent a collision
    that exists in one of them.
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

    A contract name is also a file name and a QML type name (``Items`` becomes
    ``Items.syn`` in its owner's folder, ``Items`` in QML, and ``ItemsSourceHelper`` in
    C++), so a name QML cannot use is refused here rather than at the far end of a build.

    `entity_type` is the type of the entity the file lands in, which decides whether the
    one type-specific helper name is taken. Omitted, only the always-reserved names are.
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
    """The entity block for an owner named in a command, read from the project.

    Where an entity's files go depends on what kind of entity it is, so a command given a
    bare name has to look the entity up before it can write anything beside it.
    """
    config_path = Path(project_dir) / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        config = yaml.safe_load(config_path.read_text()) or {}
    for entity in config.get("entities") or []:
        if isinstance(entity, dict) and entity.get("name") == owner:
            return entity
    raise AddContractError(f"unknown entity '{owner}'")


def _base_type(written: str) -> str:
    """A contract type without its bracketed size. `string[120]` is a string here.

    The size is a fact about the boundary and not about the value: the generated owner-side
    code refuses anything longer, and QML has no such type to declare. Writing the brackets
    into the file produced `property string[120] message`, which is not a property with a
    limit on it, it is a syntax error, and the engine refuses the whole document over it.
    """
    return str(written or "").split("[")[0].strip()


def _declaration(member: Dict[str, Any]) -> str:
    """One contract member as the QML line that declares it.

    The annotated spelling for parameters and return types (`amount: int`), which is what the
    QML coding conventions ask for and what the editor's own reader expects to find when it
    reads the file back.

    A body nobody has written yet is a `return;`, which is what a function that answers
    nothing does. The empty body it replaces (`function fetch(): var {}`) was shorter to read
    and cost a warning on every scaffold: qmlformat expands `{}` to a brace on a line of its
    own, none of the settings a project ships govern that, and so a new project reported its
    own starting files as reformattable on its first `synqt check`. The written-out form is
    what qmlformat leaves alone, measured against the scaffold's own `.qmlformat.ini`.

    A signal with no parameters loses them for the same reason: qmlformat writes
    `signal closed()` back as `signal closed`.
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
    """The declarations a contract's members are written as inside the owner's Source.

    Properties first, then signals, then the functions, one blank line between the groups and
    between one function and the next. That is the order the QML coding conventions ask for,
    and a body is three lines now, so a run of them with nothing between reads as one block
    of text. The order is this writer's, not the formatter's: a project ships
    `NormalizeOrder=false`, so qmlformat keeps whatever order it is handed.

    A model is skipped, and can only be skipped. `model rows(int id, string title)` has no QML
    declaration form, so putting a line there for one would put something in the file that QML
    would refuse to load. It reaches consumers through the generated Source helper either way.
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
    """An owner-side Source: the right root type, declaring whatever the contract says.

    A point drawn with its members already named gets a file that declares them, so the
    contract and the QML that implements it agree from the moment both are written rather
    than after somebody has copied one into the other.
    """
    declared = declarations_for(members)
    return _SOURCE_TEMPLATE.format(contract=contract, point=point,
                                   declared=f"\n{declared}\n" if declared else "")


def untouched_scaffold(text: str, entity: Dict[str, Any]) -> bool:
    """Whether this file is byte for byte what a scaffolder wrote, and nobody has edited it.

    An entity is one file, so the file it was given when it was created is the same file
    that becomes the Source of the point it exports, and exporting one has to rewrite it.
    Rewriting an author's work is never acceptable. This is the whole licence to
    do it, and any edit at all withdraws it.
    """
    from . import addentity, newproject  # here: newproject reaches addentity at import time

    name = str(entity.get("name") or "")
    return text in {newproject.entity_singleton(name),
                    addentity.entity_qml(appmodel.entity_type(entity), name)}


def write_source(project_dir: os.PathLike[str] | str, owner: Dict[str, Any], contract: str, *,
                 point: str, path: Optional[str] = None,
                 members: Optional[List[Dict[str, Any]]] = None) -> Optional[str]:
    """Write the owner-side Source for a connect point, unless the author has written one.

    Returns the project-relative path when it wrote one, and None when the file was there
    and was left alone. A connect point with no Source file is a connect point the entity
    cannot host, and it fails at start-up rather than at the moment the point was added, so
    the empty file is written with the point rather than left to be remembered.
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
    """A word about a Source file that is there but is not one.

    The likeliest way to get here is the stub `synqt add entity` writes: it demonstrates
    the blueprint's helper and is rooted at QtObject, which is not something an owner can
    host a connect point with. `synqt check` refuses it either way. Saying so now saves the
    trip.
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

    The owner is the whole name. An entity has one connect point, so `edge` exports
    the `Edge` contract from `web/edge/Edge.qml` and its consumers reach it
    as `Edge`. An entity that already has one is told to add to its `export:` block rather
    than given a second point that would replace the first.
    """
    contract = appmodel.contract_of({"owner": owner})
    owning = owner_entity(project_dir, owner)
    check_qml_name(contract, entity_type=appmodel.entity_type(owning), entity=owning)
    config_path = Path(project_dir) / "synqt.yaml"
    if not config_path.exists():
        raise AddContractError("no synqt.yaml (run 'synqt new' first)")
    config: Dict[str, Any] = yaml.safe_load(config_path.read_text()) or {}

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

    # Spliced into the text rather than dumped over it. The file is the author's, and one
    # added entry is not a reason to lose their comments and their formatting. What crosses
    # the point is written on the point, so the starter block goes in with it.
    block: Dict[str, Any] = {"owner": owner, "consumers": consumers,
                             "export": _EXPORT_TEMPLATE}
    config_path.write_text(yamledit.append_item(
        config_path.read_text(), "connect_points", block))
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
