# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add entity`` and ``synqt providers``: scaffold an entity of a given type.

An entity is instantiated with secure defaults. The embedded provider needs no
configuration. An external provider is masked behind the same entity, its secret recorded
as an ``env:`` reference (with a ``.env.example`` entry) and its connection forced to
verified TLS. The connect point Source calls only the provider interface, so the engine
choice never leaks into the rest of the system.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

from synqt import addcontract, appgen, appmodel, newproject, presets, yamledit

# Family -> the providers bundled for it (default first). This is the list the C++ family
# factories accept, and the only place it is written down: `synqt add entity` offers these
# and `synqt check` validates a provider.name against them. Anything else needs a custom
# provider registered with the ProviderRegistry and selected as custom:<Name>.
PROVIDERS: Dict[str, List[str]] = {
    "relational": ["sqlite", "postgres", "mysql"],
    "cache": ["memory", "redis"],
    "document": ["memory", "mongodb"],
}

# The selector that sends a provider name to the ProviderRegistry rather than to a bundled
# engine. `synqt check` cannot know what an entity registers (that is C++ resolved at
# start), so it validates the shape and leaves the lookup to the factory, which names the
# registered alternatives when it misses.
CUSTOM_PREFIX = "custom:"

# Entity type -> the provider family behind it, or None for a type with no data engine.
# `client` and `web_edge` are entity types too but are not offered here. A project has one
# of each, `synqt new` writes them, and a second of either is not a thing to scaffold.
TYPES: Dict[str, Optional[str]] = {
    "relational": "relational",
    "cache": "cache",
    "document": "document",
    "api": None,   # QHttpServer inbound (opt-in) + Http outbound; no data provider
    "jobs": None,      # timers + bounded queue; no data provider
    "service": None,   # a bare entity: no engine, no browser-facing side, just its QML
}

# External providers. The NAME of the environment variable the credential is read from, and
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


def _entity_qml(entity_type: str, name: str) -> str:
    """An entity's own file: the singleton it is, showing the helper its type gives it.

    Not a connect point Source. A Source is named after the point it answers, and a new
    entity has no points yet, so scaffolding one would mean inventing a name for a thing
    nobody had asked for; `synqt add connect-point` writes the Source, named after the
    contract, at the moment there is something to call it. What the author does get here
    is the entity itself with its helper demonstrated in it, which is the part that is
    the same however the points are eventually named.

    Written the way ``qmlformat`` would write it, using the project's own
    ``.qmlformat.ini``, so a scaffolded project passes its own ``synqt check`` (the
    ``check.qml_format`` rule) with nothing to reformat first.
    """
    header = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
              "// SPDX-License-Identifier: Apache-2.0\n\n"
              "pragma Singleton\n\nimport QtQuick\nimport SynQt\n\n")
    if entity_type == "relational":
        return header + (
            f"// The '{name}' entity itself. It reaches its engine through the `Db`\n"
            "// helper only (parameterized query/exec) and never names one.\n"
            "//\n"
            "// No `Caller` here, and that is not an omission: this file is the entity, not\n"
            "// a connect point, so nothing outside the entity can reach it and there is no\n"
            "// caller to authorize. Authorization lives in the Source of each connect\n"
            "// point, which is where a caller actually arrives.\n"
            "QtObject {\n"
            "    function insert(row) {\n"
            "        Db.exec(\"INSERT INTO items(text, author) VALUES(?, ?)\", "
            "[row.text, row.author]);\n"
            "    }\n"
            "}\n")
    if entity_type == "cache":
        return header + (
            f"// The '{name}' entity itself. It calls the `Cache` helper only, so the\n"
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
    if entity_type == "document":
        return header + (
            f"// The '{name}' entity itself. It calls the `Docs` helper only\n"
            "// (collection, filter and document as maps) and never names an engine. The\n"
            "// filter is built here from a value, never forwarded whole from a caller: a\n"
            "// filter map is the engine's query language the way a string is SQL's.\n"
            "//\n"
            "// No `Caller` here: this file is the entity, not a connect point, so there is\n"
            "// no caller to authorize. That check belongs in each connect point's Source.\n"
            "QtObject {\n"
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
            f"// The '{name}' entity itself: the two halves of talking to the outside.\n"
            "//\n"
            "// Outbound is `Http`, and it can reach exactly the prefixes network.outbound\n"
            "// names in synqt.yaml and nothing else. TLS is verified and plaintext is\n"
            "// refused in release, so this file never touches a socket or a certificate.\n"
            "//\n"
            "// Inbound is `Api`, and the routes below are this entity's whole public\n"
            "// surface. Who may call them was decided in synqt.yaml (the API keys, the\n"
            "// browser origins, the limits) and checked before a handler runs, so a\n"
            "// handler is about the answer and not about the caller.\n"
            "QtObject {\n"
            "    // Uncomment network.inbound in synqt.yaml and these start serving.\n"
            "    Component.onCompleted: {\n"
            "        if (typeof Api === \"undefined\") {\n"
            "            return;   // outbound only: this entity opens no port\n"
            "        }\n"
            "\n"
            "        // Return a value and it is the 200.\n"
            "        Api.get(\"/health\", () => {\n"
            "            return {\n"
            "                ok: true\n"
            "            };\n"
            "        });\n"
            "\n"
            "        // A captured `:name` segment, and a body the handler validates before\n"
            "        // it trusts it. `request.body` is the parsed JSON for a JSON request.\n"
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
            "    // Outbound, for whatever this gateway fronts. Answer later by calling\n"
            "    // request.reply(...) from the promise, which is what lets one route wait\n"
            "    // for an upstream or for a connect point before it answers.\n"
            "    function upstream(url) {\n"
            "        return Http.get(url);\n"
            "    }\n"
            "}\n")
    if entity_type == "jobs":
        return header + (
            f"// The '{name}' entity itself. Scheduling and the bounded work queue belong\n"
            "// to the `Jobs` helper, so there is no timer here to manage and nothing to deploy.\n"
            "QtObject {\n"
            "    // The rollup this entity exists to run, every minute, off the request path.\n"
            "    Component.onCompleted: Jobs.every(60000, function () {\n"
            "        console.log(\"rollup\");\n"
            "    })\n"
            "}\n")
    # An empty object on one line, which is what qmlformat writes for one: the scaffold
    # promises a project that passes its own `synqt check` with nothing to reformat, and a
    # plain service is the one type with nothing to put in the braces.
    return header + "QtObject {}\n"


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
        # Outbound with an empty allowlist and no inbound at all. The entity is closed, and
        # opening it is one edit in one place. The empty list is written rather than left
        # out so there is somewhere obvious to put the first prefix. `synqt check` says
        # nothing about it, because an allowlist that allows nothing allows nothing.
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
    # Built before the name is checked, because what the block grants is part of what the
    # name may collide with: an entity that declares `network.outbound` has `Http` in scope,
    # and one that does not may be called `http` like any other word.
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

    # Spliced into the text rather than dumped over it. The file is the author's, and one
    # added entity is not a reason to lose their comments and their formatting.
    if not config_path.exists():
        config_path.write_text("entities: []\n")
    config_path.write_text(yamledit.append_item(config_path.read_text(), "entities", block))

    # The entity folder and the entity's own file; relational gets a schema file too. No
    # Source: a Source answers a connect point and is named after it, and this entity has
    # none yet. `synqt add connect-point` writes one the moment there is a name for it.
    entity_dir = root / appmodel.entity_dir(block)
    entity_dir.mkdir(parents=True, exist_ok=True)
    own = appmodel.entity_file_path(block)
    (root / own).write_text(_entity_qml(entity_type, name))
    if entity_type == "relational":
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
    # Regenerate the buildable app. The new entity needs its main.cpp, its CMake target
    # and its preset, and a command that leaves a project needing a build before it is
    # complete is a command that half worked. `synqt build` regenerates these too, so this
    # only moves the moment, but the moment is the one the author is looking at.
    config = yaml.safe_load(config_path.read_text()) or {}
    presets.write(root, config)
    appgen.generate(root, config)

    folder = appmodel.entity_dir(block)
    steps.append(f"  - {own} is the entity itself, with the type's helper shown in it.")
    steps.append(f"  - Add the connect point(s) this entity owns: 'synqt add connect-point "
                 f"<name> --owner {name} --consumers <a,b>' writes {folder}/<Contract>.qml "
                 "and you declare what crosses it beside that.")
    return "\n".join(steps)


def list_providers() -> str:
    lines = ["Available providers per family (default first):"]
    for family, providers in PROVIDERS.items():
        lines.append(f"  {family}: {', '.join(providers)}")
    lines.append("  (entity types: relational, cache, document, api, jobs, service)")
    return "\n".join(lines)
