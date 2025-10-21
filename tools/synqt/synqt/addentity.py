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

# External providers: the NAME of the environment variable the credential is read from, and
# the provider block that references it. Nothing here ever holds a credential: `secret_env` is
# a variable name, and the block records the `env:` reference the runtime resolves at start-up
# from the entity's own environment. That distinction is why these are not called `secret`.
_EXTERNAL: Dict[str, Dict[str, Any]] = {
    "postgres": {"secret_env": "DB_PASSWORD", "block": lambda name, secret_env: {
        "name": "postgres", "host": "db.internal", "port": 5432, "database": name,
        "user": name, "password": f"env:{secret_env}", "sslmode": "verify-full",
        "ca_cert": "certs/db-ca.pem", "pool_size": 8}},
    "mysql": {"secret_env": "DB_PASSWORD", "block": lambda name, secret_env: {
        "name": "mysql", "host": "db.internal", "port": 3306, "database": name,
        "user": name, "password": f"env:{secret_env}", "sslmode": "verify-full",
        "ca_cert": "certs/db-ca.pem", "pool_size": 8}},
    "redis": {"secret_env": "REDIS_PASSWORD", "block": lambda name, secret_env: {
        "name": "redis", "host": "cache.internal", "port": 6379,
        "password": f"env:{secret_env}", "tls": True, "ca_cert": "certs/redis-ca.pem"}},
    "mongodb": {"secret_env": "MONGODB_URI", "block": lambda name, secret_env: {
        "name": "mongodb", "uri": f"env:{secret_env}", "tls": True,
        "ca_cert": "certs/mongo-ca.pem"}},
}


class AddEntityError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _source_stub(blueprint: str, name: str) -> str:
    """The Source stub for a blueprint entity.

    Written the way ``qmlformat`` would write it, using the project's own
    ``.qmlformat.ini``, so a scaffolded project passes its own ``synqt check`` (the
    ``check.qml_format`` rule) with nothing to reformat first.
    """
    header = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
              "// SPDX-License-Identifier: Apache-2.0\n\nimport QtQuick\nimport SynQt\n\n")
    if blueprint == "persistence":
        return header + (
            "// Owner of a persistence connect point. It calls the `Db` helper only\n"
            "// (parameterized query/exec) and never names an engine.\n"
            "QtObject {\n"
            "    function insert(row) {\n"
            "        if (Caller.entity !== \"web\") {\n"
            "            return;   // authorize the calling entity\n"
            "        }\n"
            "        Db.exec(\"INSERT INTO items(text, author) VALUES(?, ?)\", "
            "[row.text, row.author]);\n"
            "    }\n"
            "}\n")
    if blueprint == "cache":
        return header + (
            "// Owner of a cache connect point. It calls the `Cache` helper only, so the\n"
            "// entity works the same on the embedded store and on an external engine.\n"
            "QtObject {\n"
            "    function put(key, value) {\n"
            "        Cache.set(key, value, 300);\n"
            "    }\n"
            "\n"
            "    function fetch(key) {\n"
            "        return Cache.get(key);\n"
            "    }\n"
            "}\n")
    if blueprint == "document":
        return header + (
            "// Owner of a document connect point. It calls the `Docs` helper only\n"
            "// (collection, filter and document as maps) and never names an engine. The\n"
            "// filter is built here from a value, never forwarded whole from a caller: a\n"
            "// filter map is the engine's query language the way a string is SQL's.\n"
            "QtObject {\n"
            "    function add(doc) {\n"
            "        if (Caller.entity !== \"web\") {\n"
            "            return;   // authorize the calling entity\n"
            "        }\n"
            "        Docs.insert(\"items\", doc);\n"
            "    }\n"
            "\n"
            "    function byAuthor(author) {\n"
            "        return Docs.find(\"items\", {\n"
            "            \"author\": String(author)\n"
            "        });\n"
            "    }\n"
            "}\n")
    if blueprint == "gateway":
        return header + (
            "// Outbound only by default: it consumes external HTTP through the `Http`\n"
            "// helper (TLS-verified, plaintext refused in release) and never touches sockets.\n"
            "QtObject {\n"
            "    function upstream(url) {\n"
            "        return Http.get(url);\n"
            "    }\n"
            "}\n")
    if blueprint == "jobs":
        return header + (
            "// Owner of a jobs connect point. Scheduling and the bounded work queue belong\n"
            "// to the `Jobs` helper, so there is no timer here to manage and nothing to deploy.\n"
            "QtObject {\n"
            "    // The rollup this entity exists to run, every minute, off the request path.\n"
            "    Component.onCompleted: Jobs.every(60000, function () {\n"
            "        console.log(\"rollup\");\n"
            "    })\n"
            "}\n")
    return header + "QtObject {\n}\n"


def entity_block(name: str, blueprint: str, provider: Optional[str]) -> Dict[str, Any]:
    block: Dict[str, Any] = {"name": name, "kind": "service", "blueprint": blueprint}
    family = BLUEPRINTS.get(blueprint)
    if family:
        chosen = provider or PROVIDERS[family][0]
        if chosen in _EXTERNAL:
            block["provider"] = _EXTERNAL[chosen]["block"](name, _EXTERNAL[chosen]["secret_env"])
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

    # An external provider's credential is documented by name, with no value: the line written
    # here is `DB_PASSWORD=`, so the variable to set is discoverable and nothing is committed.
    chosen = provider or (PROVIDERS[family][0] if family else None)
    secret_env: Optional[str] = None
    if chosen in _EXTERNAL:
        secret_env = _EXTERNAL[chosen]["secret_env"]
        env_example = root / ".env.example"
        lines = env_example.read_text().splitlines() if env_example.exists() else []
        if not any(line.startswith(secret_env + "=") for line in lines):
            lines.append(f"{secret_env}=")
            env_example.write_text("\n".join(lines) + "\n")

    steps = [f"Entity '{name}' scaffolded ({blueprint}"
             + (f", provider {chosen}" if chosen else "") + ")."]
    if secret_env:
        steps.append(f"  - Put the {chosen} credential in the entity .env as {secret_env} "
                     "(never in synqt.yaml, never in a client target).")
        steps.append("  - The connection uses verified TLS by default; keep it that way "
                     "(release refuses plaintext).")
        if chosen == "mysql":
            steps.append("  - The QMYSQL plugin must be built against MariaDB Connector/C "
                         "(LGPLv2.1), never Oracle's GPLv2-only libmysqlclient (see "
                         "https://synqt.org/licensing/).")
    steps.append("  - Add the connect point(s) this entity owns under 'connect_points' "
                 "with a consumers allowlist.")
    return "\n".join(steps)


def list_providers() -> str:
    lines = ["Available providers per family (default first):"]
    for family, providers in PROVIDERS.items():
        lines.append(f"  {family}: {', '.join(providers)}")
    lines.append("  (blueprints: persistence, cache, document, gateway, jobs, service)")
    return "\n".join(lines)
