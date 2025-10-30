# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add contract`` and ``synqt add connect-point``: scaffold the typed boundary."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

from synqt import qmlscan, yamledit

_CONTRACT_TEMPLATE = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A SynQt contract: the typed shape of what may cross a connect point. Only declared
// model roles ever reach a consumer; props are READPUSH (consumers read, cannot set).
contract {name} {{
    prop int count                       // owner writes, consumers read
    model rows(int id, string text)      // only these roles cross to consumers
    slot add(string text)             // a consumer -> owner request; authorize Caller
    signal changed()                  // the owner notifies consumers
}}
"""

_SOURCE_TEMPLATE = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// Owner of the "{point}" connect point, and empty for now. Its props, models and signals
// are the ones declared in shared/{contract}.syn, and nothing undeclared ever reaches a
// consumer. A slot a consumer calls arrives here with `Caller` set to whoever called it:
// authorize that caller first, then act. This file is where the rule lives; a check in a
// consumer's UI is a courtesy, not a guard.
{contract}Source {{
    id: root
}}
"""

# The names SynQt itself puts in the QML scope an entity's own files are resolved in: the
# blueprint helpers the runtime installs (`Db`, `Cache`, ...) and the accessors registered
# into the SynQt module. A file in an entity directory becomes a QML type named after the
# file, and a type from the directory wins over one from an import, so `Cache.qml` sitting
# beside a Source would quietly shadow the cache helper that Source calls. Refused where
# somebody picks the name rather than debugged where the call goes wrong.
RESERVED_QML_NAMES = frozenset({
    "App", "Cache", "Caller", "Client", "Db", "Docs", "EntityTest", "Graphics", "Http",
    "IdentityMapping", "Jobs", "PageSeed", "Router", "Server", "Session",
})


class AddContractError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def check_qml_name(name: str) -> str:
    """`name` back, or an error saying why it cannot name a QML type.

    A contract name is also a file name and a QML type name (``Items`` becomes
    ``shared/Items.syn``, ``ItemsSource`` in QML, and ``ItemsSourceHelper`` in C++), so a
    name QML cannot use is refused here rather than at the far end of a build.
    """
    if not name or not name.isascii() or not name.isidentifier() or not name[0].isupper():
        raise AddContractError(
            f"'{name}' cannot name a QML type; use a name that starts with a capital "
            "letter and holds only letters, digits and underscores (for example Items)")
    if name in RESERVED_QML_NAMES:
        raise AddContractError(
            f"'{name}' is what SynQt calls one of the helpers an entity's QML uses, and a "
            f"{name}.qml of your own would shadow it wherever it is called; pick another "
            "name")
    return name


def source_path(owner: str, contract: str) -> str:
    """Where the owner-side Source for a connect point lives when nothing says otherwise.

    The default the runtime resolves (``topologywriter`` writes it into the entity topology
    and the edge's generated main hands it to WebEdge), so a file written at this path is
    the file that will be loaded.
    """
    return f"{owner}/{contract}.qml"


def source_stub(contract: str, point: str) -> str:
    """An empty owner-side Source: the right root type, and nothing in it yet."""
    return _SOURCE_TEMPLATE.format(contract=contract, point=point)


def write_source(project_dir: os.PathLike[str] | str, owner: str, contract: str, *,
                 point: str, path: Optional[str] = None) -> Optional[str]:
    """Write the owner-side Source for a connect point, unless there is one already.

    Returns the project-relative path when it wrote one, and None when the file was there
    and was left alone. A connect point with no Source file is a connect point the entity
    cannot host, and it fails at start-up rather than at the moment the point was added, so
    the empty file is written with the point rather than left to be remembered.
    """
    relative = path or source_path(owner, contract)
    target = Path(project_dir) / relative
    if target.exists():
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source_stub(contract, point), encoding="utf-8")
    return relative


def _root_note(project_dir: os.PathLike[str] | str, owner: str,
               contract: str) -> List[str]:
    """A word about a Source file that is there but is not one.

    The likeliest way to get here is the stub `synqt add entity` writes: it demonstrates
    the blueprint's helper and is rooted at QtObject, which is not something an owner can
    host a connect point with. `synqt check` refuses it either way; saying so now saves the
    trip.
    """
    relative = source_path(owner, contract)
    source = Path(project_dir) / relative
    root = qmlscan.root_type(source.read_text(encoding="utf-8", errors="replace"))
    if root is None or root == f"{contract}Source":
        return []
    return [f"  - {relative} is rooted at '{root}'. A connect point Source has to be "
            f"rooted at '{contract}Source'; change it, or point this connect point at "
            "another file with 'server:'."]


def scaffold_contract(project_dir: os.PathLike[str] | str, name: str) -> str:
    check_qml_name(name)
    shared = Path(project_dir) / "shared"
    shared.mkdir(parents=True, exist_ok=True)
    path = shared / f"{name}.syn"
    if path.exists():
        raise AddContractError(f"{path} already exists")
    path.write_text(_CONTRACT_TEMPLATE.format(name=name))
    return (f"Scaffolded shared/{name}.syn.\n"
            f"  - Wire it into a connect point: synqt add connect-point <name> "
            f"--contract {name} --owner <entity> --consumers <a,b>")


def scaffold_connect_point(project_dir: os.PathLike[str] | str, name: str, *,
                           owner: str, consumers: List[str], contract: str,
                           instance: str = "shared") -> str:
    if instance not in ("shared", "per_session", "per_peer"):
        raise AddContractError(
            "instance must be shared, per_session, or per_peer")
    check_qml_name(contract)
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
    if any(isinstance(cp, dict) and cp.get("name") == name for cp in connect_points):
        raise AddContractError(f"a connect point named '{name}' already exists")

    # Spliced into the text rather than dumped over it: the file is the author's, and one
    # added entry is not a reason to lose their comments and their formatting.
    config_path.write_text(yamledit.append_item(
        config_path.read_text(), "connect_points",
        {"name": name, "contract": contract, "owner": owner,
         "consumers": consumers, "instance": instance}))
    written = write_source(project_dir, owner, contract, point=name)
    steps = [f"Added connect point '{name}' (owner {owner}, "
             f"consumers {', '.join(consumers)}, instance {instance}). Deny-by-default: "
             "only listed consumers may acquire it."]
    if written:
        steps.append(f"  - Wrote {written}, empty. Fill in the slots there and authorize "
                     "Caller in every one of them.")
    else:
        steps.extend(_root_note(project_dir, owner, contract)
                     or [f"  - {source_path(owner, contract)} is already there; authorize "
                         "Caller in every slot it implements."])
    if not (Path(project_dir) / "shared" / f"{contract}.syn").exists():
        steps.append(f"  - Declare what crosses: synqt add contract {contract}")
    return "\n".join(steps)
