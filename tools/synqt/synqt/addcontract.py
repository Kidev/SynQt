# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add contract`` and ``synqt add connect-point``: scaffold the typed boundary."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

_CONTRACT_TEMPLATE = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A SynQt contract: the typed shape of what may cross a connect point. Only declared
// model roles ever reach a consumer; props are READPUSH (consumers read, cannot set).
contract {name} {{
    prop int count                    // owner writes, consumers read
    model rows(id, text)              // only these roles cross to consumers
    slot add(string text)             // a consumer -> owner request; authorize Caller
    signal changed()                  // the owner notifies consumers
}}
"""


class AddContractError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def scaffold_contract(project_dir: os.PathLike[str] | str, name: str) -> str:
    if not name or not name[0].isalpha():
        raise AddContractError("a contract name must start with a letter (e.g. Items)")
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

    connect_points: List[Dict[str, Any]] = config.setdefault("connect_points", [])
    if any(cp.get("name") == name for cp in connect_points):
        raise AddContractError(f"a connect point named '{name}' already exists")
    connect_points.append({
        "name": name, "contract": contract, "owner": owner,
        "consumers": consumers, "instance": instance,
    })
    config_path.write_text(yaml.safe_dump(config, sort_keys=False))
    return (f"Added connect point '{name}' (owner {owner}, consumers {', '.join(consumers)}, "
            f"instance {instance}). Deny-by-default: only listed consumers may acquire it.\n"
            f"  - Implement the owner Source in {owner}/ and authorize Caller in every slot.")
