# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add entity`` and ``synqt providers``: scaffold a blueprint entity.

A blueprint entity is instantiated with secure defaults: the embedded provider needs no
configuration; an external provider is masked behind the same entity, its secret recorded
as an ``env:`` reference (with a ``.env.example`` entry) and its connection forced to
verified TLS. The connect point Source calls only the provider interface, so the engine
choice never leaks into the rest of the system.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

# Family -> the providers bundled for it (default first). This is the list the C++ family
# factories accept, and the only place it is written down: `synqt add entity` offers these
# and `synqt check` validates a provider.name against them. Anything else needs a custom
# provider registered with the ProviderRegistry and selected as custom:<Name>.
PROVIDERS: Dict[str, List[str]] = {
    "persistence": ["sqlite", "postgres", "mysql"],
    "cache": ["memory", "redis"],
    "document": ["memory", "mongodb"],
}

# The selector that sends a provider name to the ProviderRegistry rather than to a bundled
# engine. `synqt check` cannot know what an entity registers (that is C++ resolved at
# start), so it validates the shape and leaves the lookup to the factory, which names the
# registered alternatives when it misses.
CUSTOM_PREFIX = "custom:"

# Blueprint -> (family or None, default provider or None).
BLUEPRINTS: Dict[str, Optional[str]] = {
    "persistence": "persistence",
    "cache": "cache",
    "document": "document",
    "gateway": None,   # QHttpServer inbound (opt-in) + Http outbound; no data provider
    "jobs": None,      # timers + bounded queue; no data provider
    "service": None,   # a bare custom entity
}

# External providers: the secret env var name and the provider block that carries it.
_EXTERNAL: Dict[str, Dict[str, Any]] = {
    "postgres": {"secret": "DB_PASSWORD", "block": lambda name, secret: {
        "name": "postgres", "host": "db.internal", "port": 5432, "database": name,
        "user": name, "password": f"env:{secret}", "sslmode": "verify-full",
        "ca_cert": "certs/db-ca.pem", "pool_size": 8}},
    "mysql": {"secret": "DB_PASSWORD", "block": lambda name, secret: {
        "name": "mysql", "host": "db.internal", "port": 3306, "database": name,
        "user": name, "password": f"env:{secret}", "sslmode": "verify-full",
        "ca_cert": "certs/db-ca.pem", "pool_size": 8}},
    "redis": {"secret": "REDIS_PASSWORD", "block": lambda name, secret: {
        "name": "redis", "host": "cache.internal", "port": 6379,
        "password": f"env:{secret}", "tls": True, "ca_cert": "certs/redis-ca.pem"}},
    "mongodb": {"secret": "MONGODB_URI", "block": lambda name, secret: {
        "name": "mongodb", "uri": f"env:{secret}", "tls": True, "ca_cert": "certs/mongo-ca.pem"}},
}


class AddEntityError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _source_stub(blueprint: str, name: str) -> str:
    header = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
              "// SPDX-License-Identifier: Apache-2.0\n\nimport QtQuick\nimport SynQt\n\n")
    if blueprint == "persistence":
        return header + (
            "// Owner of a persistence connect point. It calls the `Db` helper only\n"
            "// (parameterized query/exec) and never names an engine.\n"
            "QtObject {\n"
            "    function insert(row) {\n"
            "        if (Caller.entity !== \"web\") return;   // authorize the calling entity\n"
            "        Db.exec(\"INSERT INTO items(text, author) VALUES(?, ?)\",\n"
            "                [row.text, row.author]);\n"
            "    }\n"
            "}\n")
    if blueprint == "cache":
        return header + ("QtObject {\n"
                         "    function put(key, value) { Cache.set(key, value, 300); }\n"
                         "    function fetch(key) { return Cache.get(key); }\n"
                         "}\n")
    if blueprint == "gateway":
        return header + (
            "// Outbound only by default: it consumes external HTTP through the `Http`\n"
            "// helper (TLS-verified, plaintext refused in release) and never touches sockets.\n"
            "QtObject {\n"
            "    function upstream(url) { return Http.get(url); }\n"
            "}\n")
    if blueprint == "jobs":
        return header + ("QtObject {\n"
                         "    Component.onCompleted: Jobs.every(60000, function(){ /* rollup */ });\n"
                         "}\n")
    return header + "QtObject {\n}\n"


def entity_block(name: str, blueprint: str, provider: Optional[str]) -> Dict[str, Any]:
    block: Dict[str, Any] = {"name": name, "kind": "service", "blueprint": blueprint}
    family = BLUEPRINTS.get(blueprint)
    if family:
        chosen = provider or PROVIDERS[family][0]
        if chosen in _EXTERNAL:
            block["provider"] = _EXTERNAL[chosen]["block"](name, _EXTERNAL[chosen]["secret"])
        elif blueprint == "persistence":
            block["settings"] = {"file": f"{name}/data/app.db",
                                 "journal_mode": "wal", "busy_timeout_ms": 5000}
        else:
            block["provider"] = {"name": chosen}
    if blueprint == "gateway":
        block["inbound"] = False  # opt-in, reviewed choice
    return block


def scaffold(project_dir: os.PathLike[str] | str, name: str, blueprint: str,
             provider: Optional[str] = None) -> str:
    if blueprint not in BLUEPRINTS:
        raise AddEntityError(f"unknown blueprint '{blueprint}'; one of {sorted(BLUEPRINTS)}")
    family = BLUEPRINTS.get(blueprint)
    if provider and family and provider not in PROVIDERS[family]:
        raise AddEntityError(
            f"provider '{provider}' is not a {blueprint} provider; one of {PROVIDERS[family]}")

    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        config = yaml.safe_load(config_path.read_text()) or {}
    entities: List[Dict[str, Any]] = config.setdefault("entities", [])
    if any(isinstance(e, dict) and e.get("name") == name for e in entities):
        raise AddEntityError(f"an entity named '{name}' already exists")

    block = entity_block(name, blueprint, provider)
    entities.append(block)
    config_path.write_text(yaml.safe_dump(config, sort_keys=False))

    # The entity folder + a Source stub; persistence gets a schema file too.
    entity_dir = root / name
    entity_dir.mkdir(parents=True, exist_ok=True)
    (entity_dir / "Items.qml").write_text(_source_stub(blueprint, name))
    if blueprint == "persistence":
        (entity_dir / "schema.sql").write_text(
            "-- forward-only migrations, one statement per step\n"
            "CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "                    text TEXT NOT NULL, author TEXT NOT NULL);\n")

    # An external provider's secret is documented as unset.
    chosen = provider or (PROVIDERS[family][0] if family else None)
    secret: Optional[str] = None
    if chosen in _EXTERNAL:
        secret = _EXTERNAL[chosen]["secret"]
        env_example = root / ".env.example"
        lines = env_example.read_text().splitlines() if env_example.exists() else []
        if not any(line.startswith(secret + "=") for line in lines):
            lines.append(f"{secret}=")
            env_example.write_text("\n".join(lines) + "\n")

    steps = [f"Entity '{name}' scaffolded ({blueprint}"
             + (f", provider {chosen}" if chosen else "") + ")."]
    if secret:
        steps.append(f"  - Put the {chosen} credential in the entity .env as {secret} "
                     "(never in synqt.yaml, never in a client target).")
        steps.append("  - The connection uses verified TLS by default; keep it that way "
                     "(release refuses plaintext).")
        if chosen == "mysql":
            steps.append("  - The QMYSQL plugin must be built against MariaDB Connector/C "
                         "(LGPLv2.1), never Oracle's GPLv2-only libmysqlclient (see "
                         "docs/licensing.md).")
    steps.append("  - Add the connect point(s) this entity owns under 'connect_points' "
                 "with a consumers allowlist.")
    return "\n".join(steps)


def list_providers() -> str:
    lines = ["Available providers per family (default first):"]
    for family, providers in PROVIDERS.items():
        lines.append(f"  {family}: {', '.join(providers)}")
    lines.append("  (blueprints: persistence, cache, document, gateway, jobs, service)")
    return "\n".join(lines)
