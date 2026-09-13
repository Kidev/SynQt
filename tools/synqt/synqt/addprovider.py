# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add provider``: scaffold a custom provider implementing a family interface."""

from __future__ import annotations

import os
import re
from pathlib import Path

FAMILY_INTERFACE = {
    "relational": ("IPersistenceProvider", "ipersistenceprovider.h"),
    "cache": ("ICacheProvider", "icacheprovider.h"),
    "document": ("IDocumentProvider", "idocumentprovider.h"),
}

# Family to the macro that registers a provider with the ProviderRegistry, which is what
# lets provider.name select it.
FAMILY_REGISTER_MACRO = {
    "relational": "SYNQT_REGISTER_PERSISTENCE_PROVIDER",
    "cache": "SYNQT_REGISTER_CACHE_PROVIDER",
    "document": "SYNQT_REGISTER_DOCUMENT_PROVIDER",
}

# The family operations, stubbed. Each fails through the interface, naming itself, so an
# unfinished provider fails at its first call.
_OPERATIONS = {
    "relational": (
        "    DbResult query(const QString &sql, const QVariantList &params) override\n"
        "    {\n"
        "        // TODO: run the parameterized statement and map each row to a QVariantMap\n"
        "        // of column name -> value. Never concatenate params into sql.\n"
        "        Q_UNUSED(sql);\n"
        "        Q_UNUSED(params);\n"
        "        return DbResult::failure(notImplemented(QStringLiteral(\"query\")));\n"
        "    }\n\n"
        "    DbResult exec(const QString &sql, const QVariantList &params) override\n"
        "    {\n"
        "        // TODO: as query(), returning affected + insertId.\n"
        "        Q_UNUSED(sql);\n"
        "        Q_UNUSED(params);\n"
        "        return DbResult::failure(notImplemented(QStringLiteral(\"exec\")));\n"
        "    }\n\n"
        "    bool begin(QString *error) override { return fail(error, \"begin\"); }\n"
        "    bool commit(QString *error) override { return fail(error, \"commit\"); }\n"
        "    bool rollback(QString *error) override { return fail(error, \"rollback\"); }\n\n"
        "    bool migrate(const QStringList &steps, QString *error) override\n"
        "    {\n"
        "        // TODO: apply the steps not yet recorded, in order, and record the new\n"
        "        // version. Forward only; re-running must be a no-op.\n"
        "        Q_UNUSED(steps);\n"
        "        return fail(error, \"migrate\");\n"
        "    }\n"),
    "cache": (
        "    QVariant get(const QString &key) override\n"
        "    {\n"
        "        // TODO: return the value, or an invalid QVariant. A miss is a normal\n"
        "        // result in this family, not an error.\n"
        "        Q_UNUSED(key);\n"
        "        notImplemented(\"get\");\n"
        "        return QVariant{};\n"
        "    }\n\n"
        "    void set(const QString &key, const QVariant &value, int ttlSeconds) override\n"
        "    {\n"
        "        // TODO: store with the TTL (ttlSeconds <= 0 means no expiry).\n"
        "        Q_UNUSED(key);\n"
        "        Q_UNUSED(value);\n"
        "        Q_UNUSED(ttlSeconds);\n"
        "        notImplemented(\"set\");\n"
        "    }\n\n"
        "    void del(const QString &key) override\n"
        "    {\n"
        "        Q_UNUSED(key);\n"
        "        notImplemented(\"del\");\n"
        "    }\n\n"
        "    qint64 incr(const QString &key, qint64 by) override\n"
        "    {\n"
        "        // TODO: atomically add and return the new value.\n"
        "        Q_UNUSED(key);\n"
        "        Q_UNUSED(by);\n"
        "        notImplemented(\"incr\");\n"
        "        return 0;\n"
        "    }\n\n"
        "    void expire(const QString &key, int ttlSeconds) override\n"
        "    {\n"
        "        // TODO: set or replace the TTL on an existing key (ttlSeconds <= 0 means\n"
        "        // no expiry, as on set(), and never \"drop the key now\").\n"
        "        Q_UNUSED(key);\n"
        "        Q_UNUSED(ttlSeconds);\n"
        "        notImplemented(\"expire\");\n"
        "    }\n"),
    "document": (
        "    QVariant insert(const QString &collection, const QVariantMap &document) override\n"
        "    {\n"
        "        // TODO: insert and return the new document's id.\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(document);\n"
        "        notImplemented(\"insert\");\n"
        "        return QVariant{};\n"
        "    }\n\n"
        "    QVariantList find(const QString &collection, const QVariantMap &filter,\n"
        "                      const QVariantMap &options) override\n"
        "    {\n"
        "        // TODO: translate the filter map to your engine's query. It is a map, not\n"
        "        // a query string, so nothing here is ever concatenated user input.\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(filter);\n"
        "        Q_UNUSED(options);\n"
        "        notImplemented(\"find\");\n"
        "        return QVariantList{};\n"
        "    }\n\n"
        "    int update(const QString &collection, const QVariantMap &filter,\n"
        "               const QVariantMap &change) override\n"
        "    {\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(filter);\n"
        "        Q_UNUSED(change);\n"
        "        notImplemented(\"update\");\n"
        "        return 0;\n"
        "    }\n\n"
        "    int remove(const QString &collection, const QVariantMap &filter) override\n"
        "    {\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(filter);\n"
        "        notImplemented(\"remove\");\n"
        "        return 0;\n"
        "    }\n"),
}

# The cache and document families have no error channel (a miss or an empty result is a
# normal answer), so an unwritten operation says so in the log.
_LOGGED_HELPER = (
    "    // Until the operations above are written, every call says so in the log. This\n"
    "    // family has no error to return, and a stub that answered like an empty store\n"
    "    // would look like it worked.\n"
    "    void notImplemented(const char *operation) const\n"
    "    {\n"
    "        qWarning(\"%s: %s() is not implemented\", qUtf8Printable(name()), operation);\n"
    "    }\n\n")

# The relational family reports errors through DbResult and QString *error.
_HELPERS = {
    "relational": (
        "    // Until the operations above are written, every call returns this error through\n"
        "    // the interface. Errors are returned, never thrown across the boundary.\n"
        "    QString notImplemented(const QString &operation) const\n"
        "    {\n"
        "        return QStringLiteral(\"%1: %2() is not implemented\")\n"
        "            .arg(name(), operation);\n"
        "    }\n\n"
        "    bool fail(QString *error, const char *operation) const\n"
        "    {\n"
        "        if (error != nullptr) {\n"
        "            *error = notImplemented(QLatin1String(operation));\n"
        "        }\n"
        "        return false;\n"
        "    }\n\n"),
    "cache": _LOGGED_HELPER,
    "document": _LOGGED_HELPER,
}

_INCLUDES = {
    "relational": "#include <QString>\n#include <QStringList>\n#include <QVariant>\n"
                   "#include <QVariantList>\n",
    "cache": "#include <QString>\n#include <QVariant>\n#include <QtLogging>\n",
    "document": "#include <QString>\n#include <QVariant>\n#include <QVariantList>\n"
                "#include <QVariantMap>\n#include <QtLogging>\n",
}


#: A provider name: it becomes a C++ class (`<Name>Provider`), a file name and the string
#: `provider.name: custom:<Name>` selects.
_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")


class AddProviderError(Exception):
    """A scaffolding error surfaced to the CLI (no traceback for the user)."""


def _skeleton(name: str, family: str) -> str:
    interface, header = FAMILY_INTERFACE[family]
    macro = FAMILY_REGISTER_MACRO[family]
    return (
        "// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
        "// SPDX-License-Identifier: Apache-2.0\n\n"
        f'#include "{header}"\n'
        '#include "providerconfig.h"\n'
        '#include "providerregistry.h"\n\n'
        f"{_INCLUDES[family]}\n"
        "#include <utility>\n\n"
        "// A custom provider is entity code and gets the same review. Honour the interface\n"
        "// contract: pass parameters separately (never concatenate), report errors through\n"
        "// the return value (never throw across the boundary), take credentials from the\n"
        "// entity env only, and connect to an external engine over verified TLS (refuse\n"
        "// plaintext in release).\n\n"
        "namespace SynQt {\n\n"
        f"class {name}Provider final : public {interface}\n"
        "{\n"
        "public:\n"
        f"    explicit {name}Provider(ProviderConfig config)\n"
        "        : m_config{std::move(config)}\n"
        "    {\n"
        "    }\n\n"
        "    // TODO: open the engine here over verified TLS, with credentials from m_config\n"
        "    // (resolved from the entity env). Refuse a plaintext or unverified connection\n"
        "    // when m_config.release is true.\n"
        "    bool connect(QString *error) override\n"
        "    {\n"
        "        Q_UNUSED(error);\n"
        "        return true;\n"
        "    }\n\n"
        "    void disconnect() override {}\n\n"
        "    // TODO: report real readiness, so the entity reports \"not ready\" and retries\n"
        "    // instead of failing.\n"
        "    bool isHealthy() const override { return true; }\n\n"
        f'    QString name() const override {{ return QStringLiteral("custom:{name}"); }}\n\n'
        f"{_OPERATIONS[family]}"
        "\nprivate:\n"
        f"{_HELPERS[family]}"
        "    ProviderConfig m_config;\n"
        "};\n\n"
        "// Registers the provider under its bare name, so `provider.name:\n"
        f"// custom:{name}` in synqt.yaml selects it. Without this the entity refuses to\n"
        "// start.\n"
        f'{macro}("{name}", {name}Provider)\n\n'
        "} // namespace SynQt\n")


def scaffold(project_dir: os.PathLike[str] | str, name: str, family: str) -> str:
    if family not in FAMILY_INTERFACE:
        raise AddProviderError(
            f"unknown family '{family}'; one of {sorted(FAMILY_INTERFACE)}")
    if not _NAME.match(name or ""):
        raise AddProviderError(
            f"'{name[:80]}' cannot name a provider: it becomes the C++ class '<name>Provider' "
            "and a file name, so it starts with a letter and holds only letters, digits and "
            "underscores")

    root = Path(project_dir)
    out_dir = root / "providers" / "custom"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / f"{name.lower()}provider.cpp"
    if out_file.exists():
        raise AddProviderError(f"{out_file} already exists")
    out_file.write_text(_skeleton(name, family), encoding="utf-8")

    interface = FAMILY_INTERFACE[family][0]
    reported = "through the interface" if family == "relational" else "in the log"
    return (
        f"Custom {family} provider '{name}' scaffolded at {out_file.relative_to(root)}.\n"
        f"  - Implement the {interface} operations (parameters separate, errors returned).\n"
        f"    It compiles and registers as it is; every operation reports that it is not\n"
        f"    written yet ({reported}),\n"
        f"    so nothing fails quietly while you work.\n"
        f"  - Select it with provider.name: custom:{name} in that entity's synqt.yaml block.\n"
        f"    That selection is also what compiles this file into the entity, so the\n"
        f"    {FAMILY_REGISTER_MACRO[family]} line in it runs; there is no CMake to edit.\n"
        "  - Connect over verified TLS; keep credentials in the entity env only.")
