# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add entity`` and ``synqt providers``: scaffold an entity of a given type.

The embedded provider needs no configuration. An external provider's secret is an ``env:``
reference (with a ``.env.example`` entry) and its connection is forced to verified TLS. The
Source calls only the provider interface.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

from synqt import (addcontract, appgen, appmodel, monitorscaffold, newproject,
                   presets, yamledit)

# Family to bundled providers, default first. The list the C++ factories accept. Anything
# else is a custom provider selected as custom:<Name>.
PROVIDERS: Dict[str, List[str]] = {
    "relational": ["sqlite", "postgres", "mysql"],
    "cache": ["memory", "redis"],
    "document": ["memory", "mongodb"],
}

# The selector that sends a name to the ProviderRegistry. `synqt check` validates only its
# shape; the factory names the registered providers on a miss.
CUSTOM_PREFIX = "custom:"

# Entity type to provider family, or None. `client` and `web_edge` are written by `synqt
# new`, not here.
TYPES: Dict[str, Optional[str]] = {
    "relational": "relational",
    "cache": "cache",
    "document": "document",
    "api": None,   # QHttpServer inbound (opt-in) + Http outbound; no data provider
    "jobs": None,      # timers + bounded queue; no data provider
    "monitor": None,   # the operations console: a history, an operator gate, no data provider
    "service": None,   # a bare entity: no engine, no browser-facing side, just its QML
}

# External providers: the variable name the credential is read from (`secret_env`), and the
# provider block with its `env:` reference. Never a credential.
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


def entity_qml(entity_type: str, name: str) -> str:
    """An entity own file, showing the helper its type gives it.

    Marked ``pragma Shared`` (``synqt build`` writes ``pragma Singleton`` into the engine
    copy). `synqt add connect-point` rewrites it into the Source while it is untouched.
    Formatted as ``qmlformat`` would with the project ``.qmlformat.ini``.
    """
    header = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
              "// SPDX-License-Identifier: Apache-2.0\n\n"
              f"pragma {appmodel.SHARED_PRAGMA}\n\nimport SynQt\n\n")
    if entity_type == "relational":
        return header + (
            f"// The '{name}' entity itself. It reaches its engine only through the `Db` helper\n"
            "// (parameterized query and exec) and never names the engine.\n"
            "//\n"
            "// There is no `Caller` here: this file is the entity, not a connect point, so no\n"
            "// caller arrives here. Authorize callers in the Source of each connect point.\n"
            "QtObject {\n"
            "    id: root\n"
            "\n"
            "    function insert(row) {\n"
            "        Db.exec(\"INSERT INTO items(text, author) VALUES(?, ?)\", "
            "[row.text, row.author]);\n"
            "    }\n"
            "}\n")
    if entity_type == "cache":
        return header + (
            f"// The '{name}' entity itself. It calls only the `Cache` helper, so it works the\n"
            "// same on the embedded store and on an external engine.\n"
            "QtObject {\n"
            "    id: root\n"
            "\n"
            "    function put(key, value) {\n"
            "        Cache.set(key, value, 300);\n"
            "    }\n"
            "\n"
            "    function fetch(key) {\n"
            "        return Cache.get(key);\n"
            "    }\n"
            "}\n")
    if entity_type == "document":
        return header + (
            f"// The '{name}' entity itself. It calls only the `Docs` helper (collection, filter\n"
            "// and document as maps) and never names an engine. The filter is built here from a\n"
            "// value, never forwarded whole from a caller: a filter map is the engine's query\n"
            "// language.\n"
            "//\n"
            "// There is no `Caller` here: this file is the entity, not a connect point.\n"
            "// Authorize callers in the Source of each connect point.\n"
            "QtObject {\n"
            "    id: root\n"
            "\n"
            "    function add(doc) {\n"
            "        Docs.insert(\"items\", doc);\n"
            "    }\n"
            "\n"
            "    function byAuthor(author) {\n"
            "        return Docs.find(\"items\", {\n"
            "            \"author\": String(author)\n"
            "        });\n"
            "    }\n"
            "}\n")
    if entity_type == "api":
        return header + (
            f"// The '{name}' entity itself: its outbound and inbound HTTP.\n"
            "//\n"
            "// Outbound is `Http`. It reaches only the prefixes network.outbound names in\n"
            "// synqt.yaml, verifies TLS, and refuses plaintext in release.\n"
            "//\n"
            "// Inbound is `Api`, and the routes below are this entity's whole public surface.\n"
            "// synqt.yaml sets who may call them (API keys, browser origins, limits), and that\n"
            "// is checked before a handler runs.\n"
            "QtObject {\n"
            "    id: root\n"
            "\n"
            "    // Uncomment network.inbound in synqt.yaml to serve these routes.\n"
            "    Component.onCompleted: {\n"
            "        if (typeof Api === \"undefined\") {\n"
            "            return;   // outbound only: this entity opens no port\n"
            "        }\n"
            "\n"
            "        // A returned value is the 200 response.\n"
            "        Api.get(\"/health\", () => {\n"
            "            return {\n"
            "                ok: true\n"
            "            };\n"
            "        });\n"
            "\n"
            "        // A captured `:id` segment, and a body the handler validates.\n"
            "        // `request.body` is the parsed JSON of a JSON request.\n"
            "        Api.post(\"/things/:id\", request => {\n"
            "            if (!request.body || !request.body.value) {\n"
            "                request.fail(422, \"value is required\");\n"
            "                return;\n"
            "            }\n"
            "            return {\n"
            "                id: request.params.id,\n"
            "                value: request.body.value\n"
            "            };\n"
            "        });\n"
            "    }\n"
            "\n"
            "    // Outbound calls to what this gateway fronts. A route can answer later by\n"
            "    // calling request.reply(...) from the promise.\n"
            "    function upstream(url) {\n"
            "        return Http.get(url);\n"
            "    }\n"
            "}\n")
    if entity_type == "jobs":
        return header + (
            f"// The '{name}' entity itself. The `Jobs` helper owns the scheduling and\n"
            "// the bounded work queue.\n"
            "QtObject {\n"
            "    id: root\n"
            "\n"
            "    // Runs every minute, off the request path.\n"
            "    Component.onCompleted: Jobs.every(60000, function () {\n"
            "        console.log(\"rollup\");\n"
            "    })\n"
            "}\n")
    # A plain service carries only its id, formatted as qmlformat writes it.
    return header + "QtObject {\n    id: root\n}\n"


def entity_block(name: str, entity_type: str, provider: Optional[str]) -> Dict[str, Any]:
    block: Dict[str, Any] = {"name": name, "type": entity_type}
    family = TYPES.get(entity_type)
    if family:
        chosen = provider or PROVIDERS[family][0]
        if chosen in _EXTERNAL:
            block["provider"] = _EXTERNAL[chosen]["block"](name, _EXTERNAL[chosen]["secret_env"])
        elif entity_type == "relational":
            block["settings"] = {"file": f"{appmodel.entity_dir(block)}/data/app.db",
                                 "journal_mode": "wal", "busy_timeout_ms": 5000}
        else:
            block["provider"] = {"name": chosen}
    if entity_type == "api":
        # Outbound with an empty allowlist, no inbound: closed, with an obvious place for
        # the first prefix.
        block["network"] = {"outbound": []}
    return block


def scaffold(project_dir: os.PathLike[str] | str, name: str,
             entity_type: str = appmodel.PLAIN_TYPE,
             provider: Optional[str] = None) -> str:
    if entity_type not in TYPES:
        raise AddEntityError(f"unknown entity type '{entity_type}'; one of {sorted(TYPES)}")
    family = TYPES.get(entity_type)
    if provider and family and provider not in PROVIDERS[family]:
        raise AddEntityError(
            f"provider '{provider}' is not a {entity_type} provider; "
            f"one of {PROVIDERS[family]}")
    # Built before the name check: `Http` is only reserved when `network.outbound` is
    # declared.
    if entity_type == "monitor":
        # A monitor has its own scaffold (monitorscaffold).
        return monitorscaffold.scaffold(project_dir, name)
    block = entity_block(name, entity_type, provider)
    try:
        addcontract.check_qml_name(f"{name[:1].upper()}{name[1:]}",
                                   entity_type=entity_type, entity=block)
    except addcontract.AddContractError as error:
        raise AddEntityError(str(error)) from error

    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config: Dict[str, Any] = {}
    if config_path.exists():
        config = yaml.safe_load(config_path.read_text()) or {}
    entities: List[Dict[str, Any]] = config.get("entities") or []
    if any(isinstance(e, dict) and e.get("name") == name for e in entities):
        raise AddEntityError(f"an entity named '{name}' already exists")

    # Spliced into the text, keeping the author's comments and formatting.
    if not config_path.exists():
        config_path.write_text("entities: []\n")
    config_path.write_text(yamledit.append_item(config_path.read_text(), "entities", block))

    # The entity folder and file, plus a schema for relational. No Source until the entity
    # exports a point.
    entity_dir = root / appmodel.entity_dir(block)
    entity_dir.mkdir(parents=True, exist_ok=True)
    own = appmodel.entity_file_path(block)
    (root / own).write_text(entity_qml(entity_type, name))
    if entity_type == "relational":
        (entity_dir / "schema.sql").write_text(
            "-- forward-only migrations, one statement per step\n"
            "CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "                    text TEXT NOT NULL, author TEXT NOT NULL);\n")

    # The external credential variable, with no value (`DB_PASSWORD=`).
    chosen = provider or (PROVIDERS[family][0] if family else None)
    secret_env: Optional[str] = None
    if chosen in _EXTERNAL:
        secret_env = _EXTERNAL[chosen]["secret_env"]
        env_example = root / ".env.example"
        lines = env_example.read_text().splitlines() if env_example.exists() else []
        if not any(line.startswith(secret_env + "=") for line in lines):
            lines.append(f"{secret_env}=")
            env_example.write_text("\n".join(lines) + "\n")

    steps = [f"Entity '{name}' scaffolded ({entity_type}"
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
    # Regenerate the app so the new entity has its main.cpp, CMake target and preset.
    config = yaml.safe_load(config_path.read_text()) or {}
    presets.write(root, config)
    appgen.generate(root, config)

    folder = appmodel.entity_dir(block)
    contract = appmodel.contract_of({"owner": name})
    steps.append(f"  - {own} is the entity itself, with the type's helper shown in it.")
    steps.append(f"  - Export a connect point from it: 'synqt add connect-point {name} "
                 f"--consumers <a,b>' turns that same file into the Source, rooted at "
                 f"'{contract}', and you declare what crosses it beside that.")
    return "\n".join(steps)


def list_providers() -> str:
    lines = ["Available providers per family (default first):"]
    for family, providers in PROVIDERS.items():
        lines.append(f"  {family}: {', '.join(providers)}")
    lines.append("  (entity types: relational, cache, document, api, jobs, service)")
    return "\n".join(lines)
