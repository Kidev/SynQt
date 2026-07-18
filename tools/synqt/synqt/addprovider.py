# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt add provider``: scaffold a custom provider implementing a family interface."""

from __future__ import annotations

import os
from pathlib import Path

FAMILY_INTERFACE = {
    "persistence": ("IPersistenceProvider", "ipersistenceprovider.h"),
    "cache": ("ICacheProvider", "icacheprovider.h"),
    "document": ("IDocumentProvider", "idocumentprovider.h"),
}

# Family -> the macro that registers a provider with the ProviderRegistry. Implementing the
# interface only gets you a class; the registration is what makes provider.name able to
# select it, so the skeleton ships with it already written.
FAMILY_REGISTER_MACRO = {
    "persistence": "SYNQT_REGISTER_PERSISTENCE_PROVIDER",
    "cache": "SYNQT_REGISTER_CACHE_PROVIDER",
    "document": "SYNQT_REGISTER_DOCUMENT_PROVIDER",
}

# The family operations, stubbed. The registration below instantiates the class, so a
# skeleton that left these pure would not compile; and a stub that quietly returned success
# would be worse than not compiling. Each one fails through the interface (never throws),
# naming itself, so an unfinished provider is loud at the first call rather than mysterious.
_OPERATIONS = {
    "persistence": (
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
        "        // result in this family, not an error, so an unfinished cache is an\n"
        "        // always-miss cache: correct, just useless.\n"
        "        Q_UNUSED(key);\n"
        "        return QVariant{};\n"
        "    }\n\n"
        "    void set(const QString &key, const QVariant &value, int ttlSeconds) override\n"
        "    {\n"
        "        // TODO: store with the TTL (ttlSeconds <= 0 means no expiry).\n"
        "        Q_UNUSED(key);\n"
        "        Q_UNUSED(value);\n"
        "        Q_UNUSED(ttlSeconds);\n"
        "    }\n\n"
        "    void del(const QString &key) override { Q_UNUSED(key); }\n\n"
        "    qint64 incr(const QString &key, qint64 by) override\n"
        "    {\n"
        "        // TODO: atomically add and return the new value.\n"
        "        Q_UNUSED(key);\n"
        "        Q_UNUSED(by);\n"
        "        return 0;\n"
        "    }\n\n"
        "    void expire(const QString &key, int ttlSeconds) override\n"
        "    {\n"
        "        Q_UNUSED(key);\n"
        "        Q_UNUSED(ttlSeconds);\n"
        "    }\n"),
    "document": (
        "    QVariant insert(const QString &collection, const QVariantMap &document) override\n"
        "    {\n"
        "        // TODO: insert and return the new document's id.\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(document);\n"
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
        "        return QVariantList{};\n"
        "    }\n\n"
        "    int update(const QString &collection, const QVariantMap &filter,\n"
        "               const QVariantMap &change) override\n"
        "    {\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(filter);\n"
        "        Q_UNUSED(change);\n"
        "        return 0;\n"
        "    }\n\n"
        "    int remove(const QString &collection, const QVariantMap &filter) override\n"
        "    {\n"
        "        Q_UNUSED(collection);\n"
        "        Q_UNUSED(filter);\n"
        "        return 0;\n"
        "    }\n"),
}

# The persistence family reports errors through DbResult and QString *error both, so its
# skeleton carries the two shapes of the same "not written yet" message.
_HELPERS = {
    "persistence": (
        "    // Until the operations above are written, every call says so through the\n"
        "    // interface. Errors are returned here, never thrown across the boundary.\n"
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
    "cache": "",
    "document": "",
}

_INCLUDES = {
    "persistence": "#include <QString>\n#include <QStringList>\n#include <QVariant>\n"
                   "#include <QVariantList>\n",
    "cache": "#include <QString>\n#include <QVariant>\n",
    "document": "#include <QString>\n#include <QVariant>\n#include <QVariantList>\n"
                "#include <QVariantMap>\n",
}


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
        "// A custom provider is your code, reviewed like any entity code; the framework\n"
        "// does not weaken its boundary for it. Honour the interface contract: pass\n"
        "// parameters separately (never concatenate), report errors through the return\n"
        "// value (never throw across the boundary), keep credentials from the entity env\n"
        "// only, and connect to an external engine over verified TLS (refuse plaintext in\n"
        "// release).\n\n"
        "namespace SynQt {\n\n"
        f"class {name}Provider final : public {interface}\n"
        "{\n"
        "public:\n"
        f"    explicit {name}Provider(ProviderConfig config)\n"
        "        : m_config{std::move(config)}\n"
        "    {\n"
        "    }\n\n"
        "    // TODO: open the engine here, over verified TLS, with credentials taken from\n"
        "    // m_config (which the entity resolved from its own env). Refuse a plaintext\n"
        "    // or unverified connection when m_config.release is true.\n"
        "    bool connect(QString *error) override\n"
        "    {\n"
        "        Q_UNUSED(error);\n"
        "        return true;\n"
        "    }\n\n"
        "    void disconnect() override {}\n\n"
        "    // TODO: report real readiness, so the entity can say \"not ready\" and retry\n"
        "    // rather than crash.\n"
        "    bool isHealthy() const override { return true; }\n\n"
        f'    QString name() const override {{ return QStringLiteral("custom:{name}"); }}\n\n'
        f"{_OPERATIONS[family]}"
        "\nprivate:\n"
        f"{_HELPERS[family]}"
        "    ProviderConfig m_config;\n"
        "};\n\n"
        "// This is what makes the class selectable: it registers the provider under the\n"
        f"// bare name, so `provider.name: custom:{name}` in synqt.yaml reaches it. Without\n"
        "// it the name resolves to nothing and the entity refuses to start.\n"
        f'{macro}("{name}", {name}Provider)\n\n'
        "} // namespace SynQt\n")


def scaffold(project_dir: os.PathLike[str] | str, name: str, family: str) -> str:
    if family not in FAMILY_INTERFACE:
        raise AddProviderError(
            f"unknown family '{family}'; one of {sorted(FAMILY_INTERFACE)}")

    root = Path(project_dir)
    out_dir = root / "providers" / "custom"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / f"{name.lower()}provider.cpp"
    if out_file.exists():
        raise AddProviderError(f"{out_file} already exists")
    out_file.write_text(_skeleton(name, family))

    interface = FAMILY_INTERFACE[family][0]
    return (
        f"Custom {family} provider '{name}' scaffolded at {out_file.relative_to(root)}.\n"
        f"  - Implement the {interface} operations (parameters separate, errors returned).\n"
        f"    It compiles and registers as it is; every operation reports that it is not\n"
        f"    written yet, so nothing fails quietly while you work.\n"
        f"  - Compile the file into the entity that uses it (its CMake target), so the\n"
        f"    {FAMILY_REGISTER_MACRO[family]} line in it runs.\n"
        f"  - Select it with provider.name: custom:{name} in that entity's synqt.yaml block.\n"
        "  - Connect over verified TLS; keep credentials in the entity env only.")
